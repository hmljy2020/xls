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

#include "xls/experimental/stage_gfsim/native_expression.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"

namespace xls::stage_gfsim {

std::string NativeCodec(const Type* type) {
  if (type->IsBits()) {
    const int64_t width = type->AsBitsOrDie()->bit_count();
    return width <= 64 ? absl::StrCat("NativeBits<", width, ">") : "";
  }
  if (type->IsToken()) {
    return "NativeToken";
  }
  if (type->IsArray()) {
    const ArrayType* array = type->AsArrayOrDie();
    const std::string element = NativeCodec(array->element_type());
    if (element.empty() || array->size() == 0) {
      return "";
    }
    return absl::StrCat("NativeArray<", element, ", ", array->size(), ">");
  }
  std::string args;
  for (const Type* element : type->AsTupleOrDie()->element_types()) {
    const std::string codec = NativeCodec(element);
    if (codec.empty()) {
      return "";
    }
    absl::StrAppend(&args, args.empty() ? "" : ", ", codec);
  }
  return absl::StrCat("NativeTuple<", args, ">");
}

std::string NativeLiteral(const Type* type, const Value& value) {
  const std::string native_type = NativeCodec(type) + "::Type";
  if (value.IsToken()) {
    return native_type + "{}";
  }
  if (value.IsBits()) {
    return absl::StrCat(native_type, "{", value.bits().ToUint64().value(),
                        "ULL}");
  }
  std::string args;
  for (int64_t i = 0; i < value.size(); ++i) {
    const Type* sub = type->IsArray() ? type->AsArrayOrDie()->element_type()
                                      : type->AsTupleOrDie()->element_type(i);
    absl::StrAppend(&args, i == 0 ? "" : ", ",
                    NativeLiteral(sub, value.element(i)));
  }
  return absl::StrCat(native_type, "{", args, "}");
}

namespace {

std::string Call(std::string_view function, int64_t width,
                 std::string_view arguments) {
  return absl::StrCat(function, "<", width, ">(", arguments, ")");
}

std::string Cast(std::string_view expr) {
  return absl::StrCat("uint64_t{", expr, "}");
}

std::string Clip(const Node& node, std::string_view expr) {
  return Call("ClipBits", node.BitCountOrDie(), expr);
}

std::string Signed(const Node* operand, std::string_view expr) {
  return Call("SignedBits", operand->BitCountOrDie(), expr);
}

std::string_view BitOperator(Op op) {
  switch (op) {
    case Op::kAnd:
    case Op::kNand:
      return " & ";
    case Op::kOr:
    case Op::kNor:
      return " | ";
    case Op::kXor:
      return " ^ ";
    default:
      return "";
  }
}

std::string_view Comparison(Op op) {
  switch (op) {
    case Op::kEq:
      return " == ";
    case Op::kNe:
      return " != ";
    case Op::kULt:
    case Op::kSLt:
      return " < ";
    case Op::kULe:
    case Op::kSLe:
      return " <= ";
    case Op::kUGt:
    case Op::kSGt:
      return " > ";
    case Op::kUGe:
    case Op::kSGe:
      return " >= ";
    default:
      return "";
  }
}

std::string Join(const Node& node, const Variables& names,
                 std::string_view separator) {
  std::string expr;
  for (const Node* operand : node.operands()) {
    absl::StrAppend(&expr, expr.empty() ? "" : separator, names.at(operand));
  }
  return expr;
}

std::string Concat(const Node& node, const Variables& names) {
  int64_t shift = node.BitCountOrDie();
  std::string expr;
  for (const Node* operand : node.operands()) {
    const int64_t width = operand->BitCountOrDie();
    if (width == 0) {
      continue;
    }
    shift -= width;
    absl::StrAppend(&expr, expr.empty() ? "" : " | ", Cast(names.at(operand)));
    if (shift != 0) {
      absl::StrAppend(&expr, " << ", shift);
    }
  }
  return Clip(node, expr.empty() ? "0" : expr);
}

std::string SelectExpression(const Node& node, const Variables& names) {
  if (node.Is<PrioritySelect>()) {
    const auto* select = node.As<PrioritySelect>();
    std::string expr;
    for (size_t i = 0; i < select->cases().size(); ++i) {
      absl::StrAppend(&expr, "(", names.at(select->selector()), " >> ", i,
                      " & 1) ? ", names.at(select->get_case(i)), " : ");
    }
    return expr + names.at(select->default_value());
  }
  const auto* select = node.As<Select>();
  const auto cases = select->cases();
  std::string expr;
  size_t count = cases.size();
  if (!select->default_value()) {
    --count;
  }
  for (size_t i = 0; i < count; ++i) {
    absl::StrAppend(&expr, names.at(select->selector()), " == ", i, " ? ",
                    names.at(cases[i]), " : ");
  }
  const auto default_value = select->default_value();
  return expr + names.at(default_value ? *default_value : cases.back());
}

}  // namespace

std::string NativeExpression(const Node& node, const Variables& names) {
  auto arg = [&](size_t i) -> const std::string& {
    return names.at(node.operand(i));
  };
  if (std::string_view op = BitOperator(node.op()); !op.empty()) {
    if (node.operand_count() == 0) {
      return "";
    }
    const std::string expr = Join(node, names, op);
    return Clip(node, node.op() == Op::kNand || node.op() == Op::kNor
                          ? absl::StrCat("~(", expr, ")")
                          : expr);
  }
  if (std::string_view op = Comparison(node.op()); !op.empty()) {
    const bool is_signed = node.op() == Op::kSLt || node.op() == Op::kSLe ||
                           node.op() == Op::kSGt || node.op() == Op::kSGe;
    return Clip(node, is_signed
                          ? absl::StrCat(Signed(node.operand(0), arg(0)), op,
                                         Signed(node.operand(1), arg(1)))
                          : absl::StrCat(arg(0), op, arg(1)));
  }
  switch (node.op()) {
    case Op::kLiteral:
      return NativeLiteral(node.GetType(), node.As<Literal>()->value());
    case Op::kIdentity:
    case Op::kZeroExt:
      return node.op() == Op::kIdentity ? arg(0) : Clip(node, arg(0));
    case Op::kAfterAll:
    case Op::kMinDelay:
      return "NativeToken::Type{}";
    case Op::kTuple:
    case Op::kArray:
      return absl::StrCat(NativeCodec(node.GetType()), "::Type{",
                          Join(node, names, ", "), "}");
    case Op::kTupleIndex:
      return absl::StrCat("std::get<", node.As<TupleIndex>()->index(), ">(",
                          arg(0), ")");
    case Op::kArrayIndex: {
      std::string expr = arg(0);
      for (const Node* index : node.As<ArrayIndex>()->indices()) {
        expr = absl::StrCat("ArrayElement(", expr, ", ", names.at(index), ")");
      }
      return expr;
    }
    case Op::kArrayUpdate:
      return node.operand_count() == 2
                 ? arg(1)
                 : absl::StrCat("WithArrayElement(", Join(node, names, ", "),
                                ")");
    case Op::kBitSlice:
      if (node.BitCountOrDie() == 0) {
        return Clip(node, "0");
      }
      return Clip(node, absl::StrCat(Cast(arg(0)), " >> ",
                                     node.As<BitSlice>()->start()));
    case Op::kConcat:
      return Concat(node, names);
    case Op::kSignExt:
      return Clip(node, Signed(node.operand(0), arg(0)));
    case Op::kAdd:
      return Clip(node, absl::StrCat(Cast(arg(0)), " + ", arg(1)));
    case Op::kSub:
      return Clip(node, absl::StrCat(Cast(arg(0)), " - ", arg(1)));
    case Op::kNeg:
      return Clip(node, absl::StrCat("-", Cast(arg(0))));
    case Op::kUMul:
      return Clip(node, absl::StrCat(Cast(arg(0)), " * ", arg(1)));
    case Op::kSMul:
      return Clip(node, absl::StrCat("static_cast<uint64_t>(",
                                     Signed(node.operand(0), arg(0)), ") * ",
                                     Signed(node.operand(1), arg(1))));
    case Op::kNot:
      return Clip(node, absl::StrCat("~", Cast(arg(0))));
    case Op::kAndReduce:
      return Clip(
          node, absl::StrCat(arg(0), " == ",
                             Call("ClipBits", node.operand(0)->BitCountOrDie(),
                                  "UINT64_MAX")));
    case Op::kOrReduce:
      return Clip(node, absl::StrCat(arg(0), " != 0"));
    case Op::kXorReduce:
      return Clip(node, absl::StrCat("std::popcount(", arg(0), ") & 1"));
    case Op::kShll:
    case Op::kShrl:
    case Op::kShra: {
      std::string_view function = node.op() == Op::kShll   ? "ShiftLeft"
                                  : node.op() == Op::kShrl ? "ShiftRight"
                                                           : "ShiftRightSigned";
      return Call(function, node.BitCountOrDie(), Join(node, names, ", "));
    }
    case Op::kSel:
    case Op::kPrioritySel:
      return SelectExpression(node, names);
    case Op::kOneHotSel: {
      if (!node.GetType()->IsBits()) {
        return "";
      }
      const auto* select = node.As<OneHotSelect>();
      std::string expr;
      for (size_t i = 0; i < select->cases().size(); ++i) {
        absl::StrAppend(&expr, i == 0 ? "" : " | ", "((",
                        names.at(select->selector()), " >> ", i, " & 1) ? ",
                        names.at(select->get_case(i)), " : 0)");
      }
      return Clip(node, expr.empty() ? "0" : expr);
    }
    case Op::kGate:
      return absl::StrCat(arg(0), " ? ", arg(1), " : ",
                          NativeCodec(node.GetType()), "::Type{}");
    default:
      return "";
  }
}

}  // namespace xls::stage_gfsim
