// Copyright 2026 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/experimental/stage_gfsim/native_rewrites.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/span.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/bits.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/proc.h"
#include "xls/ir/topo_sort.h"
#include "xls/ir/value.h"

namespace xls::stage_gfsim {
namespace {

std::optional<uint64_t> LiteralBits(Node* node) {
  if (!node->Is<Literal>() || !node->GetType()->IsBits()) {
    return std::nullopt;
  }
  return node->As<Literal>()->value().bits().ToUint64().value();
}

uint64_t WidthMask(int64_t width) {
  return width == 64 ? UINT64_MAX : (uint64_t{1} << width) - 1;
}

struct KnownIndex {
  Node* base;
  uint64_t mask;
  uint64_t value;
};

KnownIndex MatchBits(Node* node, uint64_t value) {
  uint64_t mask = WidthMask(node->BitCountOrDie());
  while (node->Is<BitSlice>() && mask != 0) {
    const int64_t start = node->As<BitSlice>()->start();
    mask <<= start;
    value <<= start;
    node = node->operand(0);
  }
  return {node, mask, value};
}

// Recover an exact index match even when bit slicing has split the equality
// into several one-bit conditions.
std::optional<KnownIndex> MatchCondition(Node* node, bool asserted) {
  if (node->op() == Op::kNot) {
    return MatchCondition(node->operand(0), !asserted);
  }
  if (asserted && node->op() == Op::kEq) {
    for (int i = 0; i < 2; ++i) {
      if (auto value = LiteralBits(node->operand(i))) {
        return MatchBits(node->operand(1 - i), *value);
      }
    }
  }
  if (asserted && (node->op() == Op::kAnd || node->op() == Op::kNor)) {
    std::optional<KnownIndex> result;
    for (Node* operand : node->operands()) {
      auto match = MatchCondition(operand, node->op() == Op::kAnd);
      if (!match || (result && (result->base != match->base ||
                                ((result->value ^ match->value) & result->mask &
                                 match->mask) != 0))) {
        return std::nullopt;
      }
      if (result) {
        result->mask |= match->mask;
        result->value |= match->value;
      } else {
        result = match;
      }
    }
    return result;
  }
  return MatchBits(node, asserted ? 1 : 0);
}

struct IndexedWrite {
  Node* index;
  uint64_t offset;
  Node* value;

  bool operator==(const IndexedWrite&) const = default;
};

struct ArrayWrites {
  Node* array;
  std::vector<IndexedWrite> writes;

  bool operator==(const ArrayWrites&) const = default;
};

std::optional<ArrayWrites> MatchElement(Node* node, uint64_t slot) {
  if (!node->Is<PrioritySelect>()) {
    return std::nullopt;
  }
  auto* select = node->As<PrioritySelect>();
  Node* selector = select->selector();
  Node* fallback = select->default_value();
  const int64_t cases = select->cases().size();
  if (!fallback->Is<ArrayIndex>() || cases == 0 ||
      (cases > 1 &&
       (!selector->Is<Concat>() || selector->operand_count() != cases))) {
    return std::nullopt;
  }
  auto* read = fallback->As<ArrayIndex>();
  if (read->indices().size() != 1 ||
      LiteralBits(read->indices().front()) != slot) {
    return std::nullopt;
  }
  ArrayWrites result{.array = read->array(), .writes = {}};
  // A lower selector bit has higher priority, so apply it last.
  for (int64_t i = cases - 1; i >= 0; --i) {
    Node* condition = cases == 1 ? selector : selector->operand(cases - 1 - i);
    if (condition->BitCountOrDie() != 1) {
      return std::nullopt;
    }
    auto match = MatchCondition(condition, true);
    if (!match || match->mask != WidthMask(match->base->BitCountOrDie()) ||
        slot > match->mask) {
      return std::nullopt;
    }
    result.writes.push_back({match->base, (slot - match->value) & match->mask,
                             select->get_case(i)});
  }
  return result;
}

// Array simplification can scalarize a dynamic write to one select per
// element. Restore the indexed updates in the consumer's scheduled stage.
absl::StatusOr<bool> RestoreArrayUpdate(Proc& proc, Array& array) {
  std::optional<ArrayWrites> writes;
  for (int64_t slot = 0; slot < array.operand_count(); ++slot) {
    Node* element = array.operand(slot);
    if (element->Is<Literal>()) {
      continue;
    }
    auto match = MatchElement(element, slot);
    if (!match || match->array->GetType() != array.GetType() ||
        (writes && *writes != *match)) {
      return false;
    }
    writes = std::move(match);
  }
  if (!writes) {
    return false;
  }

  XLS_ASSIGN_OR_RETURN(int64_t stage, proc.GetStageIndex(&array));
  Node* replacement = writes->array;
  for (const IndexedWrite& write : writes->writes) {
    Node* index = write.index;
    if (write.offset != 0) {
      XLS_ASSIGN_OR_RETURN(
          Node * offset,
          proc.MakeNodeInStage<Literal>(
              stage, array.loc(),
              Value(UBits(write.offset, index->BitCountOrDie()))));
      XLS_ASSIGN_OR_RETURN(
          index, proc.MakeNodeInStage<BinOp>(stage, array.loc(), index, offset,
                                             Op::kAdd));
    }
    XLS_ASSIGN_OR_RETURN(replacement, proc.MakeNodeInStage<ArrayUpdate>(
                                          stage, array.loc(), replacement,
                                          write.value, absl::Span(&index, 1),
                                          /*assumed_in_bounds=*/false));
  }
  const int64_t index_width =
      Bits::MinBitCountUnsigned(array.operand_count() - 1);
  for (int64_t slot = 0; slot < array.operand_count(); ++slot) {
    Node* element = array.operand(slot);
    if (element->Is<Literal>()) {
      XLS_ASSIGN_OR_RETURN(Node * index, proc.MakeNodeInStage<Literal>(
                                             stage, array.loc(),
                                             Value(UBits(slot, index_width))));
      XLS_ASSIGN_OR_RETURN(replacement, proc.MakeNodeInStage<ArrayUpdate>(
                                            stage, array.loc(), replacement,
                                            element, absl::Span(&index, 1),
                                            /*assumed_in_bounds=*/true));
    }
  }
  XLS_RETURN_IF_ERROR(array.ReplaceUsesWith(replacement));
  return true;
}

}  // namespace

absl::Status SimplifyNativeProc(Proc& proc) {
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> order, TopoSort(&proc));
  bool changed = false;
  for (Node* node : order) {
    if (node->Is<Array>()) {
      XLS_ASSIGN_OR_RETURN(bool restored,
                           RestoreArrayUpdate(proc, *node->As<Array>()));
      changed |= restored;
    }
  }
  if (changed) {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      Node* node = *it;
      if (node->users().empty() && !node->Is<StateRead>() &&
          !OpIsSideEffecting(node->op()) && !proc.HasImplicitUse(node)) {
        XLS_RETURN_IF_ERROR(proc.RemoveNode(node));
      }
    }
  }
  return absl::OkStatus();
}

}  // namespace xls::stage_gfsim
