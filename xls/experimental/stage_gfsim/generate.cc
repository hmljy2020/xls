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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "xls/common/exit_status.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/common/status/status_macros.h"
#include "xls/experimental/stage_gfsim/model.h"
#include "xls/experimental/stage_gfsim/native_expression.h"
#include "xls/experimental/stage_gfsim/native_rewrites.h"
#include "xls/ir/channel.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/name_uniquer.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/package.h"
#include "xls/ir/proc.h"
#include "xls/ir/proc_elaboration.h"
#include "xls/ir/state_element.h"
#include "xls/ir/topo_sort.h"
#include "xls/ir/type.h"

namespace xls::stage_gfsim {
namespace {

struct StageDescription {
  std::vector<Node*> nodes;
  std::vector<Node*> live_out;
};

struct ProcDescription {
  const Proc* proc;
  std::vector<StageDescription> stages;
  std::vector<ChannelNode*> ports;
  std::unordered_map<const Node*, int64_t> stage_by_node;

  bool HasWork() const {
    return !ports.empty() || !proc->next_values().empty();
  }
};

struct InstanceDescription {
  std::string name;
  size_t proc;
  std::vector<size_t> channels;
};

struct ChannelDescription {
  std::string name;
  Port port;
  int depth;
  const Type* type;
};

struct ModelDescription {
  std::vector<ProcDescription> procs;
  std::vector<ChannelDescription> channels;
  std::vector<InstanceDescription> instances;
};

absl::Status Unsupported(const Node* node) {
  return absl::UnimplementedError(
      absl::StrCat(node->function_base()->name(), ": ", node->ToString()));
}

absl::StatusOr<ProcDescription> DescribeProc(Proc* proc, bool native) {
  if (!proc->IsScheduled()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Expected scheduled_proc for ", proc->name()));
  }

  ProcDescription result{
      .proc = proc,
      .stages = std::vector<StageDescription>(proc->stages().size()),
      .ports = {},
      .stage_by_node = {}};
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> order, TopoSort(proc));
  for (Node* node : order) {
    if (!node->Is<Literal>()) {
      XLS_ASSIGN_OR_RETURN(int64_t stage, proc->GetStageIndex(node));
      result.stage_by_node.emplace(node, stage);
    }
  }
  if (native) {
    // Channels and state still advance at their scheduled stages. Evaluate a
    // pure expression where it is first used and forward its operands instead.
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      Node* node = *it;
      if (!node->Is<Literal>() && !node->Is<StateRead>() && !node->Is<Next>() &&
          !OpIsSideEffecting(node->op()) && !node->users().empty()) {
        auto users = node->users();
        int64_t first_use = result.stage_by_node.at(users.front());
        for (Node* user : users.subspan(1)) {
          first_use = std::min(first_use, result.stage_by_node.at(user));
        }
        result.stage_by_node.at(node) = first_use;
      }
    }
  }
  std::unordered_set<const StateElement*> reads;
  std::unordered_set<const StateElement*> writes;
  std::vector<bool> has_send(result.stages.size());

  for (Node* node : order) {
    if (node->Is<Literal>()) {
      continue;
    }
    const int64_t stage = result.stage_by_node.at(node);
    if (node->Is<Send>()) {
      if (has_send[stage]) {
        return Unsupported(node);
      }
      has_send[stage] = true;
      result.ports.push_back(node->As<Send>());
    } else if (node->Is<Receive>()) {
      if (!node->As<Receive>()->is_blocking()) {
        return Unsupported(node);
      }
      result.ports.push_back(node->As<Receive>());
    } else if (node->Is<StateRead>()) {
      const StateRead* read = node->As<StateRead>();
      auto predicate = read->predicate();
      if (!reads.insert(read->state_element()).second ||
          (predicate.has_value() &&
           (!(*predicate)->Is<Literal>() ||
            (*predicate)->As<Literal>()->value().bits().IsZero()))) {
        return Unsupported(node);
      }
    } else if (node->Is<Next>()) {
      if (!writes.insert(node->As<Next>()->state_element()).second) {
        return Unsupported(node);
      }
    } else if (OpIsSideEffecting(node->op()) && node->op() != Op::kGate) {
      return Unsupported(node);
    }

    result.stages[stage].nodes.push_back(node);
    int64_t last_use = stage;
    for (Node* user : node->users()) {
      last_use = std::max(last_use, result.stage_by_node.at(user));
    }
    for (int64_t i = stage; i < last_use; ++i) {
      result.stages[i].live_out.push_back(node);
    }
  }
  return result;
}

absl::StatusOr<ModelDescription> DescribeModel(Package& package,
                                               bool native = false) {
  XLS_ASSIGN_OR_RETURN(Proc * top, package.GetTopAsProc());
  XLS_ASSIGN_OR_RETURN(
      ProcElaboration elaboration,
      package.ChannelsAreProcScoped()
          ? ProcElaboration::Elaborate(top)
          : ProcElaboration::ElaborateOldStylePackage(&package));
  ModelDescription model;
  std::unordered_map<ChannelInstance*, size_t> channels;
  std::unordered_map<Proc*, size_t> procs;
  std::vector<int> producers;
  std::vector<int> consumers;

  for (ChannelInstance* instance : elaboration.channel_instances()) {
    Channel* channel = instance->channel;
    const bool external = package.ChannelsAreProcScoped()
                              ? elaboration.IsTopInterfaceChannel(instance)
                              : channel->CanSend() != channel->CanReceive();
    Port port = Port::kInternal;
    if (external) {
      port = channel->CanReceive() ? Port::kInput : Port::kOutput;
    }
    const auto* stream = dynamic_cast<const StreamingChannel*>(channel);
    const std::string name =
        external ? std::string(channel->name()) : instance->ToString();
    if (stream == nullptr ||
        stream->flow_control() != FlowControl::kReadyValid) {
      return absl::UnimplementedError(
          absl::StrCat("Expected a streaming ready/valid channel: ", name));
    }
    const auto& config = stream->channel_config().fifo_config();
    const int64_t depth = stream->GetFifoDepth().value_or(2);
    if (depth < 1 || depth > 512 || (config && config->bypass()) ||
        !channel->initial_values().empty()) {
      return absl::UnimplementedError(
          absl::StrCat("Expected an empty, buffered channel: ", name));
    }
    channels.emplace(instance, model.channels.size());
    model.channels.push_back(
        {name, port, static_cast<int>(depth), channel->type()});
    producers.push_back(port == Port::kInput ? 1 : 0);
    consumers.push_back(port == Port::kOutput ? 1 : 0);
  }

  for (ProcInstance* instance : elaboration.proc_instances()) {
    Proc* proc = instance->proc();
    auto [entry, inserted] = procs.try_emplace(proc, model.procs.size());
    if (inserted) {
      XLS_ASSIGN_OR_RETURN(ProcDescription spec, DescribeProc(proc, native));
      model.procs.push_back(std::move(spec));
    }
    const ProcDescription& description = model.procs[entry->second];
    if (!description.HasWork()) {
      continue;
    }
    InstanceDescription spec{
        .name = instance->GetName(), .proc = entry->second, .channels = {}};
    for (const ChannelNode* operation : description.ports) {
      XLS_ASSIGN_OR_RETURN(
          ChannelInstance * channel,
          instance->GetChannelInstance(operation->channel_name()));
      const size_t index = channels.at(channel);
      spec.channels.push_back(index);
      int& endpoints =
          operation->Is<Send>() ? producers[index] : consumers[index];
      ++endpoints;
    }
    model.instances.push_back(std::move(spec));
  }

  for (size_t i = 0; i < model.channels.size(); ++i) {
    if (producers[i] != 1 || consumers[i] != 1) {
      return absl::UnimplementedError(
          absl::StrCat("Expected one send and one receive endpoint: ",
                       model.channels[i].name));
    }
  }
  return model;
}

std::string Quote(std::string_view string) {
  return absl::StrCat("\"", absl::CEscape({string.data(), string.size()}),
                      "\"");
}

const Type* PortType(const ChannelNode& port) {
  if (port.Is<Send>()) {
    return port.As<Send>()->data()->GetType();
  }
  return port.GetType()->AsTupleOrDie()->element_type(1);
}

std::string ChannelCodec(const Type* type, bool native) {
  return native ? NativeCodec(type) : "ValueCodec";
}

std::string CppType(const Type* type, bool native) {
  return native ? NativeCodec(type) + "::Type" : "Value";
}

bool SupportsNative(const ModelDescription& model) {
  for (const ChannelDescription& channel : model.channels) {
    if (NativeCodec(channel.type).empty()) {
      return false;
    }
  }
  for (const ProcDescription& proc : model.procs) {
    if (!proc.HasWork()) {
      continue;
    }
    Variables names;
    for (const Node* node : proc.proc->nodes()) {
      if (NativeCodec(node->GetType()).empty()) {
        return false;
      }
      names.emplace(node, node->GetName());
    }
    for (const StateElement* state : proc.proc->StateElements()) {
      if (NativeCodec(state->type()).empty()) {
        return false;
      }
    }
    for (const Node* node : proc.proc->nodes()) {
      if (!(node->Is<Receive>() || node->Is<Send>() || node->Is<StateRead>() ||
            node->Is<Next>()) &&
          NativeExpression(*node, names).empty()) {
        return false;
      }
    }
  }
  return true;
}

struct StageSlot {
  std::string type;
  std::string member;
  Variables fields;
};

struct StateMembers {
  std::string value;
  bool has_write = false;
  bool has_feedback = false;
  bool resident_read = false;
  bool buffered = false;

  std::string Read() const {
    return buffered ? absl::StrCat(value, ".Current()") : value;
  }
  std::string Pending() const { return absl::StrCat("next_", value); }
  std::string Available() const { return absl::StrCat("available_", value); }
  std::string NextAvailable() const {
    return absl::StrCat("next_available_", value);
  }
};

using States = std::unordered_map<const StateElement*, StateMembers>;

bool ResidentRead(const Node* node, const States& states) {
  return node->Is<StateRead>() &&
         states.at(node->As<StateRead>()->state_element()).resident_read;
}

std::string CppName(NameUniquer& names, std::string_view prefix,
                    std::string_view ir_name) {
  std::string name(prefix);
  for (char c : ir_name) {
    if (absl::ascii_isalnum(c)) {
      name += c;
    } else if (name.back() != '_') {
      name += '_';
    }
  }
  if (name.back() == '_') {
    name += "value";
  }
  return names.GetSanitizedUniqueName(name);
}

std::string Operands(const Node& node, const Variables& names,
                     std::string_view suffix = "") {
  std::string args;
  for (const Node* operand : node.operands()) {
    if (!args.empty()) {
      args += ", ";
    }
    absl::StrAppend(&args, names.at(operand), suffix);
  }
  return args;
}

std::string_view BitApi(Op op) {
  switch (op) {
    case Op::kAdd:
      return "Add";
    case Op::kSub:
      return "Sub";
    case Op::kUMul:
      return "UMul";
    case Op::kSMul:
      return "SMul";
    case Op::kUDiv:
      return "UDiv";
    case Op::kSDiv:
      return "SDiv";
    case Op::kUMod:
      return "UMod";
    case Op::kSMod:
      return "SMod";
    case Op::kNeg:
      return "Negate";
    case Op::kNot:
      return "Not";
    case Op::kConcat:
      return "Concat";
    case Op::kAnd:
      return "NaryAnd";
    case Op::kOr:
      return "NaryOr";
    case Op::kXor:
      return "NaryXor";
    case Op::kNand:
      return "NaryNand";
    case Op::kNor:
      return "NaryNor";
    case Op::kAndReduce:
      return "AndReduce";
    case Op::kOrReduce:
      return "OrReduce";
    case Op::kXorReduce:
      return "XorReduce";
    case Op::kReverse:
      return "Reverse";
    case Op::kSignExt:
      return "SignExtend";
    case Op::kZeroExt:
      return "ZeroExtend";
    default:
      return "";
  }
}

std::string LiteralExpression(const Literal& node) {
  const Value& value = node.value();
  if (value.IsToken()) {
    return "Value::Token()";
  }
  if (value.IsBits()) {
    const Bits& bits = value.bits();
    if (auto number = bits.ToUint64(); number.ok()) {
      return absl::StrCat("Value(UBits(", *number, "ULL, ", bits.bit_count(),
                          "))");
    }
  }
  return absl::StrCat("stage.Literal(", Quote(node.GetName()), ")");
}

std::string DirectExpression(const Node& node, const Variables& names) {
  switch (node.op()) {
    case Op::kLiteral:
      return LiteralExpression(*node.As<Literal>());
    case Op::kIdentity:
      return names.at(node.operand(0));
    case Op::kAfterAll:
    case Op::kMinDelay:
      return "Value::Token()";
    case Op::kTuple:
      return absl::StrCat("Value::Tuple({", Operands(node, names), "})");
    case Op::kTupleIndex:
      return absl::StrCat(names.at(node.operand(0)), ".element(",
                          node.As<TupleIndex>()->index(), ")");
    case Op::kBitSlice:
      return absl::StrCat("Value(", names.at(node.operand(0)), ".bits().Slice(",
                          node.As<BitSlice>()->start(), ", ",
                          node.BitCountOrDie(), "))");
    default:
      break;
  }

  std::string_view api = BitApi(node.op());
  if (api.empty()) {
    return "";
  }
  std::string args = Operands(node, names, ".bits()");
  if (node.Is<NaryOp>() || node.Is<Concat>()) {
    args = absl::StrCat("{", args, "}");
  } else if (node.Is<ExtendOp>()) {
    absl::StrAppend(&args, ", ", node.BitCountOrDie());
  }
  std::string expr = absl::StrCat("bits_ops::", api, "(", args, ")");
  if (node.op() == Op::kUMul || node.op() == Op::kSMul) {
    const int64_t product_width =
        node.operand(0)->BitCountOrDie() + node.operand(1)->BitCountOrDie();
    const int64_t result_width = node.BitCountOrDie();
    if (result_width < product_width) {
      absl::StrAppend(&expr, ".Slice(0, ", result_width, ")");
    } else if (result_width > product_width) {
      expr = absl::StrCat(node.op() == Op::kSMul ? "bits_ops::SignExtend("
                                                 : "bits_ops::ZeroExtend(",
                          expr, ", ", result_width, ")");
    }
  }
  return absl::StrCat("Value(", expr, ")");
}

std::string Predicate(const Node* predicate, const Variables& names,
                      bool native) {
  const std::string& name = names.at(predicate);
  return native ? name : "!" + name + ".bits().IsZero()";
}

std::string NativePredicate(const ChannelNode& node, const Variables& names) {
  const std::optional<Node*> predicate = node.Is<Receive>()
                                             ? node.As<Receive>()->predicate()
                                             : node.As<Send>()->predicate();
  return predicate ? names.at(*predicate) : "";
}

void EmitNativeReceive(std::ostream& output, const Receive& receive,
                       const Variables& names, const std::string& port,
                       size_t stage) {
  const std::string predicate = NativePredicate(receive, names);
  output << "    if (" << (predicate.empty() ? "" : predicate + " && ")
         << "!proc_.InputReady(" << stage << ", " << port << "_.readable)) {\n"
         << "      return;\n"
         << "    }\n"
         << "    const " << CppType(receive.GetType(), true) << " "
         << names.at(&receive) << "{NativeToken::Type{}, ";
  if (!predicate.empty()) {
    output << predicate << " ? ";
  }
  output << port << "_.Peek()";
  if (!predicate.empty()) {
    output << " : " << CppType(PortType(receive), true) << "{}";
  }
  output << "};\n";
}

void EmitNativeTransfers(std::ostream& output, const StageDescription& stage,
                         const Variables& ports, const Variables& names) {
  const Send* send = nullptr;
  for (const Node* node : stage.nodes) {
    if (node->Is<Receive>()) {
      const std::string predicate =
          NativePredicate(*node->As<Receive>(), names);
      output << "    ";
      if (!predicate.empty()) {
        output << "if (" << predicate << ") ";
      }
      output << ports.at(node)
             << "_.template CommitRead<kTrace>(*this, \"receive\");\n";
    } else if (node->Is<Send>()) {
      send = node->As<Send>();
    }
  }
  if (send) {
    const std::string predicate = NativePredicate(*send, names);
    output << "    ";
    if (!predicate.empty()) {
      output << "if (" << predicate << ") ";
    }
    output << ports.at(send) << "_.template CommitWrite<kTrace>(*this, "
           << names.at(send->data()) << ");\n";
  }
}

void EmitNode(std::ostream& output, const Node& node, const Variables& names,
              const Variables& ports, const States& states, NameUniquer& unique,
              bool native, size_t stage, const Send* early_send) {
  const std::string& name = names.at(&node);
  const char* attribute =
      node.GetType()->IsToken() || node.Is<StateRead>() || node.users().empty()
          ? "[[maybe_unused]] "
          : "";
  if (node.Is<Receive>()) {
    if (native) {
      EmitNativeReceive(output, *node.As<Receive>(), names, ports.at(&node),
                        stage);
      return;
    }
    const std::string pending = unique.GetSanitizedUniqueName(name + "_result");
    output << "    const auto " << pending << " = stage.Receive("
           << ports.at(&node) << "_, " << Quote(node.GetName());
    if (auto predicate = node.As<Receive>()->predicate()) {
      output << ", " << Predicate(*predicate, names, native);
    }
    output << ");\n"
           << "    if (!" << pending << ".has_value()) {\n"
           << "      return;\n"
           << "    }\n"
           << "    const Value& " << name << " = *" << pending << ";\n";
    return;
  }
  if (node.Is<StateRead>()) {
    const StateMembers& state =
        states.at(node.As<StateRead>()->state_element());
    if (state.has_feedback) {
      output << "    if (!proc_.StateReady(" << stage << ", "
             << state.Available() << ")) {\n"
             << "      return;\n"
             << "    }\n";
    }
    output << "    " << attribute << "const auto& " << name << " = "
           << state.Read() << ";\n";
    return;
  }
  if (node.Is<Send>()) {
    const Send* send = node.As<Send>();
    if (native && send != early_send) {
      const std::string predicate = NativePredicate(*send, names);
      output << "    if (" << (predicate.empty() ? "" : predicate + " && ")
             << "!proc_.OutputReady(" << stage << ", " << ports.at(&node)
             << "_.writable)) {\n";
    } else if (!native) {
      output << "    if (!stage.Send(" << ports.at(&node) << "_, "
             << names.at(send->data());
      if (auto predicate = send->predicate()) {
        output << ", " << Predicate(*predicate, names, false);
      }
      output << ")) {\n";
    }
    if (send != early_send) {
      output << "      return;\n"
             << "    }\n";
    }
    if (!node.users().empty()) {
      output << "    " << attribute << "const auto " << name << " = "
             << (native ? "NativeToken::Type{}" : "Value::Token()") << ";\n";
    }
    return;
  }
  if (node.Is<Next>()) {
    if (!node.users().empty()) {
      output << "    " << attribute << "const auto " << name << " = "
             << (native ? "NativeTuple<>::Type{}" : "Value::Tuple({})")
             << ";\n";
    }
    return;
  }

  std::string expr =
      native ? NativeExpression(node, names) : DirectExpression(node, names);
  if (expr.empty()) {
    expr = absl::StrCat("stage.Interpret(", Quote(node.GetName()), ", {",
                        Operands(node, names), "})");
  }
  output << "    " << attribute << "const auto " << name << " = " << expr
         << ";\n";
}

void EmitStateProposals(std::ostream& output, const StageDescription& stage,
                        const States& states, const Variables& names,
                        bool native) {
  for (const Node* node : stage.nodes) {
    if (node->Is<StateRead>()) {
      const StateMembers& state =
          states.at(node->As<StateRead>()->state_element());
      if (state.has_feedback) {
        output << "    " << state.NextAvailable() << " = false;\n";
      }
    } else if (node->Is<Next>()) {
      const Next* next = node->As<Next>();
      const StateMembers& state = states.at(next->state_element());
      const std::string assignment =
          state.buffered ? absl::StrCat(state.value, ".Propose(",
                                        names.at(next->value()), ");\n")
                         : absl::StrCat(state.Pending(), " = ",
                                        names.at(next->value()), ";\n");
      if (auto predicate = next->predicate()) {
        output << "    if (" << Predicate(*predicate, names, native) << ") {\n"
               << "      " << assignment << "    }\n";
      } else {
        output << "    " << assignment;
      }
      if (state.has_feedback) {
        output << "    " << state.NextAvailable() << " = true;\n";
      }
    }
  }
}

// Place single-use, stage-local computations inside the arm that uses them.
// In particular, an unselected arm need not construct any array values.
std::unordered_set<const Node*> LazySelectOperands(
    const StageDescription& stage) {
  std::unordered_set<const Node*> local(stage.nodes.begin(), stage.nodes.end());
  std::unordered_set<const Node*> lazy;
  for (auto it = stage.nodes.rbegin(); it != stage.nodes.rend(); ++it) {
    const Node* node = *it;
    if (node->users().size() != 1 || node->Is<StateRead>() ||
        OpIsSideEffecting(node->op())) {
      continue;
    }
    const Node* user = node->users().front();
    if (!local.contains(user) ||
        std::count(user->operands().begin(), user->operands().end(), node) !=
            1) {
      continue;
    }
    const bool is_case = (user->Is<Select>() || user->Is<PrioritySelect>() ||
                          user->Is<OneHotSelect>() || user->Is<Gate>()) &&
                         user->operand(0) != node;
    if (is_case || lazy.contains(user)) {
      lazy.insert(node);
    }
  }
  return lazy;
}

struct StageDispatch {
  const Node* input = nullptr;
  std::vector<uint64_t> values;
};

// Specialize on a stage input tested against several constants. Each arm can
// fold all of the dependent logic at C++ compile time. The default arm still
// handles every other input value.
StageDispatch FindStageDispatch(const StageDescription& stage,
                                const StageSlot& input) {
  std::unordered_map<const Node*, std::vector<uint64_t>> constants;
  for (const Node* node : stage.nodes) {
    if (node->op() != Op::kEq && node->op() != Op::kNe) {
      continue;
    }
    for (int i = 0; i < 2; ++i) {
      const Node* operand = node->operand(i);
      const Node* other = node->operand(1 - i);
      if (operand->GetType()->IsBits() && input.fields.contains(operand) &&
          other->Is<Literal>()) {
        constants[operand].push_back(
            other->As<Literal>()->value().bits().ToUint64().value());
      }
    }
  }
  StageDispatch best;
  for (auto& [operand, values] : constants) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.size() < 4 || values.size() > 12) {
      continue;
    }
    if (values.size() > best.values.size() ||
        (values.size() == best.values.size() &&
         operand->id() < best.input->id())) {
      best = {operand, std::move(values)};
    }
  }
  return best;
}

void EmitStage(std::ostream& output, const StageDescription& stage,
               size_t index, const std::vector<StageSlot>& slots,
               const Variables& ports, const States& states, bool native) {
  const StageSlot* input = index == 0 ? nullptr : &slots[index - 1];
  const StageSlot* next = index == slots.size() ? nullptr : &slots[index];
  const std::string input_valid =
      input ? absl::StrCat("next_valid_.test(", index - 1, ")") : "true";
  const std::string output_ready =
      next ? absl::StrCat("!next_valid_.test(", index, ")") : "true";
  const StageDispatch dispatch =
      native && input ? FindStageDispatch(stage, *input) : StageDispatch{};
  const Send* early_send = nullptr;
  if (native) {
    // Check an unconditional send before its pure computation when there
    // are no receives or state reads that could report a stall first.
    for (const Node* node : stage.nodes) {
      if (node->Is<Receive>() || node->Is<StateRead>()) {
        early_send = nullptr;
        break;
      }
      if (node->Is<Send>() && !node->As<Send>()->predicate()) {
        early_send = node->As<Send>();
      }
    }
  }
  output << "  void WorkStage" << index << "() {\n"
         << "    if (!proc_.StageReady(" << index << ", " << input_valid
         << ",\n"
         << "                          " << output_ready << ")) {\n"
         << "      return;\n"
         << "    }\n";
  if (early_send) {
    output << "    if (!proc_.OutputReady(" << index << ", "
           << ports.at(early_send) << "_.writable)) {\n"
           << "      return;\n"
           << "    }\n";
  }
  if (dispatch.input) {
    output << "    switch (" << input->member << "."
           << input->fields.at(dispatch.input) << ") {\n";
    for (uint64_t value : dispatch.values) {
      output << "      case " << value << "ULL:\n"
             << "        ExecuteStage" << index << "<true, " << value
             << "ULL>();\n"
             << "        return;\n";
    }
    output << "      default:\n"
           << "        ExecuteStage" << index << "<false, 0>();\n"
           << "        return;\n"
           << "    }\n"
           << "  }\n\n"
           << "  template <bool kKnown, uint64_t kValue>\n"
           << "  void ExecuteStage" << index << "() {\n";
  }
  if (!native) {
    output << "    StageContext stage(proc_, " << index << ");\n";
  }
  NameUniquer unique("_");
  Variables names;
  std::unordered_set<const Node*> local(stage.nodes.begin(), stage.nodes.end());
  auto emit_input = [&](const Node* node) {
    if (local.contains(node) || names.contains(node)) {
      return;
    }
    const std::string name = CppName(unique, "v_", node->GetName());
    names.emplace(node, name);
    output << (node->GetType()->IsToken() ? "    [[maybe_unused]] " : "    ");
    if (node->Is<Literal>()) {
      output << "const auto " << name << " = "
             << (native ? NativeExpression(*node, names)
                        : LiteralExpression(*node->As<Literal>()))
             << ";\n";
    } else if (node == dispatch.input) {
      output << "const auto " << name << " = kKnown ? static_cast<"
             << CppType(node->GetType(), true)
             << ">(kValue) : " << input->member << "." << input->fields.at(node)
             << ";\n";
    } else if (ResidentRead(node, states)) {
      output << "const auto& " << name << " = "
             << states.at(node->As<StateRead>()->state_element()).Read()
             << ";\n";
    } else {
      output << "const auto& " << name << " = " << input->member << "."
             << input->fields.at(node) << ";\n";
    }
  };
  for (const Node* node : stage.nodes) {
    for (const Node* operand : node->operands()) {
      emit_input(operand);
    }
  }
  for (const Node* live : stage.live_out) {
    if (!ResidentRead(live, states)) {
      emit_input(live);
    }
  }
  const auto lazy =
      native ? LazySelectOperands(stage) : std::unordered_set<const Node*>{};
  for (const Node* node : stage.nodes) {
    if (lazy.contains(node)) {
      names.emplace(node, "(" + NativeExpression(*node, names) + ")");
      continue;
    }
    names.emplace(node, CppName(unique, "v_", node->GetName()));
    EmitNode(output, *node, names, ports, states, unique, native, index,
             early_send);
  }
  if (native) {
    EmitNativeTransfers(output, stage, ports, names);
    output << "    proc_.StageFired(" << index << ");\n";
  } else {
    output << "    stage.Finish();\n";
  }
  EmitStateProposals(output, stage, states, names, native);
  if (input) {
    output << "    next_valid_.reset(" << index - 1 << ");\n";
  }
  if (next) {
    output << "    next_valid_.set(" << index << ");\n";
    for (const Node* live : stage.live_out) {
      if (auto field = next->fields.find(live); field != next->fields.end()) {
        output << "    " << next->member << "." << field->second << " = "
               << names.at(live) << ";\n";
      }
    }
  }
  output << "  }\n\n";
}

void EmitReset(std::ostream& output, const Proc& proc, const States& states,
               bool native) {
  output << "  void Reset() override {\n"
         << "    proc_.Reset();\n"
         << "    valid_.reset();\n"
         << "    next_valid_.reset();\n";
  for (const StateElement* element : proc.StateElements()) {
    const StateMembers& state = states.at(element);
    output << "    " << state.value << (state.buffered ? ".Reset(" : " = ");
    if (native) {
      output << NativeCodec(element->type()) << "::FromValue(";
    }
    output << "proc_.InitialState(" << Quote(element->name())
           << (native ? ")" : "") << ")" << (state.buffered ? ")" : "")
           << ";\n";
    if (state.has_write && !state.buffered) {
      output << "    " << state.Pending() << ".reset();\n";
    }
    if (state.has_feedback) {
      output << "    " << state.Available() << " = true;\n"
             << "    " << state.NextAvailable() << " = true;\n";
    }
  }
  output << "  }\n\n";
}

void EmitXfer(std::ostream& output, const Proc& proc, const States& states) {
  output << "  void Xfer() override {\n"
         << "    valid_ = next_valid_;\n";
  for (const StateElement* element : proc.StateElements()) {
    const StateMembers& state = states.at(element);
    if (state.buffered) {
      output << "    " << state.value << ".Xfer();\n";
    } else if (state.has_write) {
      output << "    if (" << state.Pending() << ") {\n"
             << "      " << state.value << " = std::move(*" << state.Pending()
             << ");\n"
             << "      " << state.Pending() << ".reset();\n"
             << "    }\n";
    }
    if (state.has_feedback) {
      output << "    " << state.Available() << " = " << state.NextAvailable()
             << ";\n";
    }
  }
  output << "  }\n\n";
}

void EmitProc(std::ostream& output, const ProcDescription& proc,
              std::string_view name, bool native) {
  NameUniquer unique("_");
  States states;
  for (const StateElement* element : proc.proc->StateElements()) {
    std::string name = CppName(unique, "state_", element->name());
    states.emplace(element, StateMembers{.value = absl::StrCat(name, "_"),
                                         .resident_read = native});
  }
  for (Next* write : proc.proc->next_values()) {
    StateElement* element = write->state_element();
    StateMembers& state = states.at(element);
    state.has_write = true;
    state.buffered = native && element->type()->GetFlatBitCount() >= 512;
    const int64_t write_stage = proc.proc->GetStageIndex(write).value();
    const auto reads = proc.proc->GetStateReadsByStateElement(element);
    state.has_feedback =
        !reads.empty() &&
        proc.proc->GetStageIndex(reads.front()).value() < write_stage;
    // The read locks this state until the writer advances. All users through
    // the write stage can read the current state member instead of forwarding
    // a copy, since the writer publishes its update only during Xfer.
    state.resident_read =
        native && state.has_feedback &&
        std::ranges::all_of(reads.front()->users(), [&](Node* user) {
          return proc.stage_by_node.at(user) <= write_stage;
        });
  }
  std::vector<StageSlot> slots;
  for (size_t i = 1; i < proc.stages.size(); ++i) {
    StageSlot slot{.type = absl::StrCat("Stage", i, "Input"),
                   .member = absl::StrCat("s", i - 1, "_to_s", i, "_"),
                   .fields = {}};
    NameUniquer fields("_");
    for (const Node* node : proc.stages[i - 1].live_out) {
      if (!ResidentRead(node, states)) {
        slot.fields.emplace(node, CppName(fields, "v_", node->GetName()));
      }
    }
    slots.push_back(std::move(slot));
  }

  Variables ports;
  if (native) {
    output << "template <bool kTrace>\n";
  }
  output << "class " << name << " final : public gfsim::SimModule {\n"
         << " public:\n"
         << "  " << name << "(std::string name, Proc& ir";
  for (const ChannelNode* port : proc.ports) {
    const std::string param = CppName(unique, port->Is<Send>() ? "out_" : "in_",
                                      port->channel_name());
    ports.emplace(port, param);
    output << ",\n      TypedChannel<" << ChannelCodec(PortType(*port), native)
           << ">& " << param;
  }
  output << ")\n"
         << "      : SimModule(std::move(name)), proc_(*this, ir, "
         << proc.stages.size() << ", " << (native ? "false" : "true") << ")";
  for (const ChannelNode* port : proc.ports) {
    const std::string& param = ports.at(port);
    output << ",\n        " << param << "_(" << param << ")";
  }
  output << " {}\n\n"
         << "  void Build() override { SetObjectCapacity(0); }\n\n";
  EmitReset(output, *proc.proc, states, native);
  output << "  void Work() override {\n"
         << "    proc_.BeginCycle();\n"
         << "    next_valid_ = valid_;\n"
         << "    // Downstream has finished reading before upstream refills a "
            "slot.\n";
  for (size_t i = proc.stages.size(); i-- > 0;) {
    output << "    WorkStage" << i << "();\n";
  }
  output << "  }\n\n";
  EmitXfer(output, *proc.proc, states);
  output << "  bool Progressed() const { return proc_.Progressed(); }\n"
         << "  void ReportStat() override { proc_.ReportStat(Report()); }\n\n"
         << " private:\n"
         << "  std::bitset<" << slots.size() << "> valid_;\n"
         << "  std::bitset<" << slots.size() << "> next_valid_;\n\n";
  for (size_t i = 0; i < slots.size(); ++i) {
    const StageSlot& slot = slots[i];
    output << "  struct " << slot.type << " {\n";
    for (const Node* node : proc.stages[i].live_out) {
      if (auto field = slot.fields.find(node); field != slot.fields.end()) {
        output << "    " << CppType(node->GetType(), native) << " "
               << field->second << ";\n";
      }
    }
    output << "  } " << slot.member << ";\n\n";
  }
  for (const StateElement* element : proc.proc->StateElements()) {
    const StateMembers& state = states.at(element);
    const std::string type = CppType(element->type(), native);
    if (state.buffered) {
      output << "  BufferedState<" << type << "> " << state.value << ";\n";
    } else {
      output << "  " << type << " " << state.value << ";\n";
    }
    if (state.has_write && !state.buffered) {
      output << "  std::optional<" << type << "> " << state.Pending() << ";\n";
    }
    if (state.has_feedback) {
      output << "  bool " << state.Available() << " = true;\n"
             << "  bool " << state.NextAvailable() << " = true;\n";
    }
    output << "\n";
  }
  for (size_t i = 0; i < proc.stages.size(); ++i) {
    EmitStage(output, proc.stages[i], i, slots, ports, states, native);
  }
  output << "  ProcRuntime proc_;\n";
  for (const ChannelNode* port : proc.ports) {
    output << "  TypedChannel<" << ChannelCodec(PortType(*port), native)
           << ">& " << ports.at(port) << "_;\n";
  }
  output << "};\n\n";
}

void EmitNetwork(std::ostream& output, const ModelDescription& model,
                 const std::vector<std::string>& classes,
                 const std::vector<std::string>& ir_names, bool native) {
  output << "const std::array<ChannelSpec, " << model.channels.size()
         << "> kChannels{{\n";
  for (const ChannelDescription& channel : model.channels) {
    const char* port = "kInternal";
    if (channel.port == Port::kInput) {
      port = "kInput";
    } else if (channel.port == Port::kOutput) {
      port = "kOutput";
    }
    output << "    {.name = " << Quote(channel.name)
           << ", .port = Port::" << port << ", .depth = " << channel.depth
           << "},\n";
  }
  output << "}};\n\n"
         << "template <bool kTrace>\n"
         << "class Network final : public NetworkRuntime {\n public:\n"
         << "  Network(gfsim::SimSystem& system, const Stimuli& inputs,\n"
         << "          const RunOptions& options";
  for (const std::string& ir_name : ir_names) {
    if (!ir_name.empty()) {
      output << ", Proc& " << ir_name;
    }
  }
  output << ")\n      : NetworkRuntime(system)";
  for (size_t i = 0; i < model.channels.size(); ++i) {
    output << ",\n        channel_" << i << "_(kChannels[" << i
           << "], options, Cycle())";
  }
  for (size_t i = 0; i < model.instances.size(); ++i) {
    const InstanceDescription& instance = model.instances[i];
    output << ",\n        proc_" << i << "_(std::make_shared<"
           << classes[instance.proc] << (native ? "<kTrace>" : "") << ">("
           << Quote(instance.name) << ", " << ir_names[instance.proc];
    for (size_t channel : instance.channels) {
      output << ", channel_" << channel << "_";
    }
    output << "))";
  }
  output << " {\n";
  for (size_t i = 0; i < model.channels.size(); ++i) {
    if (model.channels[i].port == Port::kInput) {
      output << "    channel_" << i << "_.LoadInputs(inputs);\n";
    }
  }
  output << "  }\n\n  void Build() override {\n"
         << "    SetObjectCapacity(" << model.instances.size() << ");\n";
  for (size_t i = 0; i < model.instances.size(); ++i) {
    output << "    AddSubmodule(proc_" << i << "_);\n";
  }
  output << "    BuildChildren();\n  }\n\n"
         << "  void Reset() override {\n";
  for (size_t i = 0; i < model.channels.size(); ++i) {
    output << "    channel_" << i << "_.Reset();\n";
  }
  output << "    NetworkRuntime::Reset();\n  }\n\n"
         << "  void Work() override { WorkCycle<true>(); }\n\n"
         << "  void RunCompiled(uint64_t cycles) override {\n"
         << "    for (uint64_t i = 0; i < cycles; ++i) {\n"
         << "      WorkCycle<false>();\n"
         << "      Xfer();\n"
         << "    }\n"
         << "  }\n\n"
         << "  template <bool kTrackProgress>\n"
         << "  void WorkCycle() {\n"
         << "    AdvanceCycle();\n";
  for (size_t i = 0; i < model.channels.size(); ++i) {
    output << "    channel_" << i << "_.Sample();\n";
  }
  output << "    bool progressed = false;\n";
  for (size_t i = 0; i < model.instances.size(); ++i) {
    output << "    proc_" << i << "_->Work();\n"
           << "    progressed |= proc_" << i << "_->Progressed();\n";
  }
  for (size_t i = 0; i < model.channels.size(); ++i) {
    if (model.channels[i].port != Port::kInternal) {
      output << "    progressed |= "
             << (model.channels[i].port == Port::kInput ? "Source" : "Sink")
             << "<kTrace>(channel_" << i << "_);\n";
    }
  }
  output << "    if constexpr (kTrackProgress) {\n"
         << "      if (progressed) {\n"
         << "        MarkProgress();\n"
         << "      }\n"
         << "    }\n"
         << "  }\n\n  void Xfer() override {\n";
  for (size_t i = 0; i < model.instances.size(); ++i) {
    output << "    proc_" << i << "_->Xfer();\n";
  }
  output << "  }\n\n private:\n";
  for (size_t i = 0; i < model.channels.size(); ++i) {
    output << "  TypedChannel<" << ChannelCodec(model.channels[i].type, native)
           << "> channel_" << i << "_;\n";
  }
  for (size_t i = 0; i < model.instances.size(); ++i) {
    output << "  std::shared_ptr<" << classes[model.instances[i].proc]
           << (native ? "<kTrace>" : "") << "> proc_" << i << "_;\n";
  }
  output << "};\n\n"
         << "absl::StatusOr<std::shared_ptr<NetworkRuntime>> BuildNetwork(\n"
         << "    gfsim::SimSystem& system, Package& package, "
            "const Stimuli& inputs,\n"
         << "    const RunOptions& options) {\n";
  for (size_t i = 0; i < model.procs.size(); ++i) {
    if (!ir_names[i].empty()) {
      output << "  XLS_ASSIGN_OR_RETURN(Proc* " << ir_names[i] << ",\n"
             << "                       package.GetProc("
             << Quote(model.procs[i].proc->name()) << "));\n";
    }
  }
  for (std::string_view trace : {"true", "false"}) {
    if (trace == "true") {
      output << "  if (options.trace) {\n  ";
    }
    output << "  return std::make_shared<Network<" << trace
           << ">>(system, inputs, options";
    for (const std::string& ir_name : ir_names) {
      if (!ir_name.empty()) {
        output << ", *" << ir_name;
      }
    }
    output << ");\n";
    if (trace == "true") {
      output << "  }\n";
    }
  }
  output << "}\n\n";
}

std::string EmitModel(const ModelDescription& model, bool native) {
  std::ostringstream output;
  output << "// Generated from XLS scheduled_proc IR.\n"
         << "#include <array>\n"
         << "#include <bitset>\n"
         << "#include <memory>\n"
         << "#include <optional>\n"
         << "#include <string>\n"
         << "#include <utility>\n\n"
         << "#include \"absl/status/statusor.h\"\n"
         << "#include \"xls/common/status/status_macros.h\"\n"
         << "#include \"xls/experimental/stage_gfsim/model.h\"\n"
         << "#include \"xls/experimental/stage_gfsim/native_value.h\"\n"
         << "#include \"xls/experimental/stage_gfsim/proc_runtime.h\"\n"
         << "#include \"xls/ir/bits_ops.h\"\n"
         << "#include \"xls/ir/package.h\"\n\n"
         << "namespace xls::stage_gfsim {\nnamespace {\n\n";

  NameUniquer unique("_");
  std::vector<std::string> classes(model.procs.size());
  std::vector<std::string> ir_names(model.procs.size());
  for (size_t i = 0; i < model.procs.size(); ++i) {
    const ProcDescription& proc = model.procs[i];
    if (proc.HasWork()) {
      classes[i] = CppName(unique, "Proc_", proc.proc->name());
      ir_names[i] = CppName(unique, "ir_", proc.proc->name());
      EmitProc(output, proc, classes[i], native);
    }
  }
  EmitNetwork(output, model, classes, ir_names, native);
  output << "const ModelSpec kModel{kChannels, BuildNetwork};\n\n"
         << "}  // namespace\n}  // namespace xls::stage_gfsim\n\n"
         << "int main(int argc, char** argv) {\n"
         << "  return xls::stage_gfsim::RunModel(argc, argv, "
            "xls::stage_gfsim::kModel);\n"
         << "}\n";
  return std::move(output).str();
}

absl::Status Generate(std::string_view input, std::string_view output) {
  XLS_ASSIGN_OR_RETURN(std::string text, GetFileContents(input));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Package> package,
                       Parser::ParsePackage(text, input));
  XLS_ASSIGN_OR_RETURN(ModelDescription model, DescribeModel(*package));
  const bool native = SupportsNative(model);
  if (native) {
    for (const auto& proc : package->procs()) {
      if (proc->IsScheduled()) {
        XLS_RETURN_IF_ERROR(SimplifyNativeProc(*proc));
      }
    }
    XLS_ASSIGN_OR_RETURN(model, DescribeModel(*package, true));
  }
  return SetFileContents(output, EmitModel(model, native));
}

}  // namespace
}  // namespace xls::stage_gfsim

int main(int argc, char** argv) {
  constexpr std::string_view kUsage = "generate scheduled.ir model.cc";
  auto args = xls::InitXls(kUsage, argc, argv);
  if (args.size() != 2) {
    return xls::ExitStatus(absl::InvalidArgumentError(kUsage));
  }
  return xls::ExitStatus(xls::stage_gfsim::Generate(args[0], args[1]));
}
