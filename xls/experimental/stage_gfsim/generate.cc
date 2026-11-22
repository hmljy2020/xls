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

  bool HasWork() const {
    return !ports.empty() || !proc->next_values().empty();
  }
};

struct InstanceDescription {
  std::string name;
  size_t proc;
  std::vector<std::string> channels;
};

struct ModelDescription {
  std::vector<ProcDescription> procs;
  std::vector<ChannelSpec> channels;
  std::vector<InstanceDescription> instances;
};

absl::Status Unsupported(const Node* node) {
  return absl::UnimplementedError(
      absl::StrCat(node->function_base()->name(), ": ", node->ToString()));
}

absl::StatusOr<ProcDescription> DescribeProc(Proc* proc) {
  if (!proc->IsScheduled()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Expected scheduled_proc for ", proc->name()));
  }

  ProcDescription result{
      .proc = proc,
      .stages = std::vector<StageDescription>(proc->stages().size()),
      .ports = {}};
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> order, TopoSort(proc));
  std::unordered_set<const StateElement*> reads;
  std::unordered_set<const StateElement*> writes;
  std::vector<bool> has_send(result.stages.size());

  for (Node* node : order) {
    if (!proc->IsStaged(node) && node->Is<Literal>()) {
      continue;
    }
    XLS_ASSIGN_OR_RETURN(int64_t stage, proc->GetStageIndex(node));
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
      XLS_ASSIGN_OR_RETURN(int64_t use, proc->GetStageIndex(user));
      if (use > last_use) {
        last_use = use;
      }
    }
    for (int64_t i = stage; i < last_use; ++i) {
      result.stages[i].live_out.push_back(node);
    }
  }
  return result;
}

absl::StatusOr<ModelDescription> DescribeModel(Package& package) {
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
    model.channels.push_back({name, port, static_cast<int>(depth)});
    producers.push_back(port == Port::kInput ? 1 : 0);
    consumers.push_back(port == Port::kOutput ? 1 : 0);
  }

  for (ProcInstance* instance : elaboration.proc_instances()) {
    Proc* proc = instance->proc();
    auto [entry, inserted] = procs.try_emplace(proc, model.procs.size());
    if (inserted) {
      XLS_ASSIGN_OR_RETURN(ProcDescription spec, DescribeProc(proc));
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
      spec.channels.push_back(model.channels[index].name);
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

using Variables = std::unordered_map<const Node*, std::string>;

struct StageSlot {
  std::string type;
  std::string member;
  Variables fields;

  std::string Pending() const { return absl::StrCat("next_", member); }
  std::string Consumed() const { return absl::StrCat("consumed_", member); }
};

struct StateMembers {
  std::string value;
  bool has_write = false;
  bool has_feedback = false;

  std::string Pending() const { return absl::StrCat("next_", value); }
  std::string Available() const { return absl::StrCat("available_", value); }
  std::string NextAvailable() const {
    return absl::StrCat("next_available_", value);
  }
};

using States = std::unordered_map<const StateElement*, StateMembers>;

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

std::string PredicateArg(std::optional<Node*> predicate,
                         const Variables& names) {
  return predicate
             ? absl::StrCat(", !", names.at(*predicate), ".bits().IsZero()")
             : "";
}

void EmitNode(std::ostream& output, const Node& node, const Variables& names,
              const Variables& ports, const States& states,
              NameUniquer& unique) {
  const std::string& name = names.at(&node);
  const char* attribute = node.GetType()->IsToken() || node.users().empty()
                              ? "[[maybe_unused]] "
                              : "";
  if (node.Is<Receive>()) {
    const std::string pending = unique.GetSanitizedUniqueName(name + "_result");
    output << "    const auto " << pending << " = stage.Receive("
           << ports.at(&node) << "_, " << Quote(node.GetName())
           << PredicateArg(node.As<Receive>()->predicate(), names) << ");\n"
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
      output << "    if (!stage.StateReady(" << state.Available() << ")) {\n"
             << "      return;\n"
             << "    }\n";
    }
    output << "    " << attribute << "const Value& " << name << " = "
           << state.value << ";\n";
    return;
  }
  if (node.Is<Send>()) {
    const Send* send = node.As<Send>();
    output << "    if (!stage.Send(" << ports.at(&node) << "_, "
           << names.at(send->data()) << PredicateArg(send->predicate(), names)
           << ")) {\n"
           << "      return;\n"
           << "    }\n";
    if (!node.users().empty()) {
      output << "    " << attribute << "const Value " << name
             << " = Value::Token();\n";
    }
    return;
  }
  if (node.Is<Next>()) {
    if (!node.users().empty()) {
      output << "    " << attribute << "const Value " << name
             << " = Value::Tuple({});\n";
    }
    return;
  }

  std::string expr = DirectExpression(node, names);
  if (expr.empty()) {
    expr = absl::StrCat("stage.Interpret(", Quote(node.GetName()), ", {",
                        Operands(node, names), "})");
  }
  output << "    " << attribute << "const Value " << name << " = " << expr
         << ";\n";
}

void EmitStateProposals(std::ostream& output, const StageDescription& stage,
                        const States& states, const Variables& names) {
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
          absl::StrCat(state.Pending(), " = ", names.at(next->value()), ";\n");
      if (auto predicate = next->predicate()) {
        output << "    if (!" << names.at(*predicate) << ".bits().IsZero()) {\n"
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

void EmitStage(std::ostream& output, const StageDescription& stage,
               size_t index, const std::vector<StageSlot>& slots,
               const Variables& ports, const States& states) {
  const StageSlot* input = index == 0 ? nullptr : &slots[index - 1];
  const StageSlot* next = index == slots.size() ? nullptr : &slots[index];
  const std::string input_valid =
      input ? absl::StrCat(input->member, ".has_value()") : "true";
  const std::string output_ready =
      next ? absl::StrCat("!", next->member, " || ", next->Consumed()) : "true";
  output << "  void WorkStage" << index << "() {\n"
         << "    if (!proc_.StageReady(" << index << ", " << input_valid
         << ",\n"
         << "                          " << output_ready << ")) {\n"
         << "      return;\n"
         << "    }\n"
         << "    StageContext stage(proc_, " << index << ");\n";
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
      output << "const Value " << name << " = "
             << LiteralExpression(*node->As<Literal>()) << ";\n";
    } else {
      output << "const Value& " << name << " = " << input->member << "->"
             << input->fields.at(node) << ";\n";
    }
  };
  for (const Node* node : stage.nodes) {
    for (const Node* operand : node->operands()) {
      emit_input(operand);
    }
  }
  for (const Node* live : stage.live_out) {
    emit_input(live);
  }
  for (const Node* node : stage.nodes) {
    names.emplace(node, CppName(unique, "v_", node->GetName()));
    EmitNode(output, *node, names, ports, states, unique);
  }
  output << "    stage.Finish();\n";
  EmitStateProposals(output, stage, states, names);
  if (input) {
    output << "    " << input->Consumed() << " = true;\n";
  }
  if (next) {
    output << "    " << next->Pending() << " = " << next->type << "{";
    if (!stage.live_out.empty()) {
      output << "\n";
      for (const Node* live : stage.live_out) {
        output << "        ." << next->fields.at(live) << " = "
               << names.at(live) << ",\n";
      }
      output << "    ";
    }
    output << "};\n";
  }
  output << "  }\n\n";
}

void EmitReset(std::ostream& output, const Proc& proc,
               const std::vector<StageSlot>& slots, const States& states) {
  output << "  void Reset() override {\n"
         << "    proc_.Reset();\n";
  for (const StageSlot& slot : slots) {
    output << "    " << slot.member << ".reset();\n"
           << "    " << slot.Pending() << ".reset();\n"
           << "    " << slot.Consumed() << " = false;\n";
  }
  for (const StateElement* element : proc.StateElements()) {
    const StateMembers& state = states.at(element);
    output << "    " << state.value << " = proc_.InitialState("
           << Quote(element->name()) << ");\n";
    if (state.has_write) {
      output << "    " << state.Pending() << ".reset();\n";
    }
    if (state.has_feedback) {
      output << "    " << state.Available() << " = true;\n"
             << "    " << state.NextAvailable() << " = true;\n";
    }
  }
  output << "  }\n\n";
}

void EmitXfer(std::ostream& output, const Proc& proc,
              const std::vector<StageSlot>& slots, const States& states) {
  output << "  void Xfer() override {\n";
  for (const StageSlot& slot : slots) {
    output << "    if (" << slot.Pending() << " || " << slot.Consumed()
           << ") {\n"
           << "      " << slot.member << " = std::exchange(" << slot.Pending()
           << ", std::nullopt);\n"
           << "    }\n"
           << "    " << slot.Consumed() << " = false;\n";
  }
  for (const StateElement* element : proc.StateElements()) {
    const StateMembers& state = states.at(element);
    if (state.has_write) {
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
              std::string_view name) {
  NameUniquer unique("_");
  std::vector<StageSlot> slots;
  for (size_t i = 1; i < proc.stages.size(); ++i) {
    StageSlot slot{.type = absl::StrCat("Stage", i, "Input"),
                   .member = absl::StrCat("s", i - 1, "_to_s", i, "_"),
                   .fields = {}};
    NameUniquer fields("_");
    for (const Node* node : proc.stages[i - 1].live_out) {
      slot.fields.emplace(node, CppName(fields, "v_", node->GetName()));
    }
    slots.push_back(std::move(slot));
  }
  States states;
  for (const StateElement* element : proc.proc->StateElements()) {
    std::string name = CppName(unique, "state_", element->name());
    states.emplace(element, StateMembers{.value = absl::StrCat(name, "_")});
  }
  for (Next* write : proc.proc->next_values()) {
    StateElement* element = write->state_element();
    StateMembers& state = states.at(element);
    state.has_write = true;
    const int64_t write_stage = proc.proc->GetStageIndex(write).value();
    const auto reads = proc.proc->GetStateReadsByStateElement(element);
    state.has_feedback =
        !reads.empty() &&
        proc.proc->GetStageIndex(reads.front()).value() < write_stage;
  }

  Variables ports;
  output << "class " << name << " final : public gfsim::SimModule {\n"
         << " public:\n"
         << "  " << name << "(std::string name, Proc& ir";
  for (const ChannelNode* port : proc.ports) {
    const std::string param = CppName(unique, port->Is<Send>() ? "out_" : "in_",
                                      port->channel_name());
    ports.emplace(port, param);
    output << ",\n      const SimChannel& " << param;
  }
  output << ")\n"
         << "      : SimModule(std::move(name)), proc_(*this, ir, "
         << proc.stages.size() << ")";
  for (const ChannelNode* port : proc.ports) {
    const std::string& param = ports.at(port);
    output << ",\n        " << param << "_(" << param << ")";
  }
  output << " {\n";
  for (const ChannelNode* port : proc.ports) {
    output << "    " << (port->Is<Send>() ? "BindOutput" : "BindInput") << "("
           << ports.at(port) << "_.queue);\n";
  }
  output << "  }\n\n"
         << "  void Build() override { SetObjectCapacity(0); }\n\n";
  EmitReset(output, *proc.proc, slots, states);
  output << "  void Work() override {\n"
         << "    // Run downstream first so an upstream stage can refill a "
            "freed slot.\n";
  for (size_t i = proc.stages.size(); i-- > 0;) {
    output << "    WorkStage" << i << "();\n";
  }
  output << "  }\n\n";
  EmitXfer(output, *proc.proc, slots, states);
  output << "  void ReportStat() override { proc_.ReportStat(Report()); }\n\n"
         << " private:\n";
  for (size_t i = 0; i < slots.size(); ++i) {
    const StageSlot& slot = slots[i];
    output << "  struct " << slot.type << " {\n";
    for (const Node* node : proc.stages[i].live_out) {
      output << "    Value " << slot.fields.at(node) << ";\n";
    }
    output << "  };\n"
           << "  std::optional<" << slot.type << "> " << slot.member << ";\n"
           << "  std::optional<" << slot.type << "> " << slot.Pending() << ";\n"
           << "  bool " << slot.Consumed() << " = false;\n\n";
  }
  for (const StateElement* element : proc.proc->StateElements()) {
    const StateMembers& state = states.at(element);
    output << "  Value " << state.value << ";\n";
    if (state.has_write) {
      output << "  std::optional<Value> " << state.Pending() << ";\n";
    }
    if (state.has_feedback) {
      output << "  bool " << state.Available() << " = true;\n"
             << "  bool " << state.NextAvailable() << " = true;\n";
    }
    output << "\n";
  }
  for (size_t i = 0; i < proc.stages.size(); ++i) {
    EmitStage(output, proc.stages[i], i, slots, ports, states);
  }
  output << "  ProcRuntime proc_;\n";
  for (const ChannelNode* port : proc.ports) {
    output << "  const SimChannel& " << ports.at(port) << "_;\n";
  }
  output << "};\n\n";
}

std::string EmitModel(const ModelDescription& model) {
  std::ostringstream output;
  output << "// Generated from XLS scheduled_proc IR.\n"
         << "#include <memory>\n"
         << "#include <optional>\n"
         << "#include <string>\n"
         << "#include <utility>\n\n"
         << "#include \"absl/status/status.h\"\n"
         << "#include \"xls/common/status/status_macros.h\"\n"
         << "#include \"xls/experimental/stage_gfsim/model.h\"\n"
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
      EmitProc(output, proc, classes[i]);
    }
  }
  output << "absl::Status BuildProcs(gfsim::SimModule& network, Package& "
            "package,\n"
         << "                        const Channels& channels) {\n";
  for (size_t i = 0; i < model.procs.size(); ++i) {
    if (!classes[i].empty()) {
      output << "  XLS_ASSIGN_OR_RETURN(Proc* " << ir_names[i] << ",\n"
             << "                       package.GetProc("
             << Quote(model.procs[i].proc->name()) << "));\n";
    }
  }
  for (const InstanceDescription& instance : model.instances) {
    output << "  network.AddSubmodule(std::make_shared<"
           << classes[instance.proc] << ">(\n      " << Quote(instance.name)
           << ",\n      *" << ir_names[instance.proc];
    for (const std::string& channel : instance.channels) {
      output << ",\n      *channels.at(" << Quote(channel) << ")";
    }
    output << "));\n";
  }
  output << "  return absl::OkStatus();\n}\n\n"
         << "const ModelSpec kModel{\n    .channels = {\n";
  for (const ChannelSpec& channel : model.channels) {
    const char* port = "kInternal";
    if (channel.port == Port::kInput) {
      port = "kInput";
    } else if (channel.port == Port::kOutput) {
      port = "kOutput";
    }
    output << "        {.name = " << Quote(channel.name)
           << ", .port = Port::" << port << ", .depth = " << channel.depth
           << "},\n";
  }
  output << "    },\n    .proc_count = " << model.instances.size()
         << ",\n    .build_procs = BuildProcs,\n"
         << "};\n\n}  // namespace\n}  // namespace xls::stage_gfsim\n\n"
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
  return SetFileContents(output, EmitModel(model));
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
