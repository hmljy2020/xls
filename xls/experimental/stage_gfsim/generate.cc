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
};

struct ModelDescription {
  std::vector<ProcDescription> procs;
  std::vector<ChannelSpec> channels;
  std::vector<InstanceSpec> instances;
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
      .stages = std::vector<StageDescription>(proc->stages().size())};
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
    } else if (node->Is<Receive>()) {
      if (!node->As<Receive>()->is_blocking()) {
        return Unsupported(node);
      }
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
  std::unordered_set<Proc*> procs;
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
    if (procs.insert(proc).second) {
      XLS_ASSIGN_OR_RETURN(ProcDescription spec, DescribeProc(proc));
      model.procs.push_back(std::move(spec));
    }
    InstanceSpec spec{
        .name = instance->GetName(), .proc = proc->name(), .bindings = {}};
    bool has_state = false;
    for (Node* node : proc->nodes()) {
      has_state |= node->Is<Next>();
      if (!node->Is<Send>() && !node->Is<Receive>()) {
        continue;
      }
      const auto* operation = static_cast<const ChannelNode*>(node);
      XLS_ASSIGN_OR_RETURN(
          ChannelInstance * channel,
          instance->GetChannelInstance(operation->channel_name()));
      const size_t index = channels.at(channel);
      spec.bindings.push_back({node->GetName(), model.channels[index].name});
      int& endpoints = node->Is<Send>() ? producers[index] : consumers[index];
      ++endpoints;
    }
    if (!spec.bindings.empty() || has_state) {
      model.instances.push_back(std::move(spec));
    }
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
              NameUniquer& unique) {
  const std::string& name = names.at(&node);
  const char* declaration = node.GetType()->IsToken() || node.users().empty()
                                ? "[[maybe_unused]] const Value "
                                : "const Value ";
  if (node.Is<Receive>() || node.Is<StateRead>()) {
    std::string call;
    if (node.Is<Receive>()) {
      call = absl::StrCat("stage.Receive(", Quote(node.GetName()),
                          PredicateArg(node.As<Receive>()->predicate(), names),
                          ")");
    } else {
      call = absl::StrCat("stage.ReadState(",
                          Quote(node.As<StateRead>()->state_element()->name()),
                          ")");
    }
    const std::string pending = unique.GetSanitizedUniqueName(name + "_result");
    output << "  const auto " << pending << " = " << call << ";\n"
           << "  if (!" << pending << ") {\n"
           << "    return absl::OkStatus();\n"
           << "  }\n"
           << "  const Value& " << name << " = *" << pending << ";\n";
    return;
  }
  if (node.Is<Send>()) {
    const Send* send = node.As<Send>();
    output << "  if (!stage.Send(" << Quote(node.GetName()) << ", "
           << names.at(send->data()) << PredicateArg(send->predicate(), names)
           << ")) {\n"
           << "    return absl::OkStatus();\n"
           << "  }\n";
    if (!node.users().empty()) {
      output << "  " << declaration << name << " = Value::Token();\n";
    }
    return;
  }
  if (node.Is<Next>()) {
    const Next* next = node.As<Next>();
    output << "  stage.WriteState(" << Quote(next->state_element()->name())
           << ", " << names.at(next->value())
           << PredicateArg(next->predicate(), names) << ");\n";
    if (!node.users().empty()) {
      output << "  " << declaration << name << " = Value::Tuple({});\n";
    }
    return;
  }

  const std::string expr = DirectExpression(node, names);
  if (expr.empty()) {
    output << "  XLS_ASSIGN_OR_RETURN(" << declaration << name
           << ", stage.Interpret(" << Quote(node.GetName()) << ", {"
           << Operands(node, names) << "}));\n";
  } else {
    output << "  " << declaration << name << " = " << expr << ";\n";
  }
}

void EmitStage(std::ostream& output, const StageDescription& stage,
               std::string_view function) {
  output << "absl::Status " << function << "(StageContext& stage) {\n";
  NameUniquer unique("_");
  Variables names;
  std::unordered_set<const Node*> local(stage.nodes.begin(), stage.nodes.end());
  auto emit_input = [&](const Node* node) {
    if (local.contains(node) || names.contains(node)) {
      return;
    }
    const std::string name = CppName(unique, "v_", node->GetName());
    names.emplace(node, name);
    output << (node->GetType()->IsToken() ? "  [[maybe_unused]] " : "  ")
           << "const Value " << name << " = ";
    if (node->Is<Literal>()) {
      output << LiteralExpression(*node->As<Literal>());
    } else {
      output << "stage.Input(" << Quote(node->GetName()) << ")";
    }
    output << ";\n";
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
    EmitNode(output, *node, names, unique);
  }
  for (const Node* live : stage.live_out) {
    output << "  stage.Forward(" << Quote(live->GetName()) << ", "
           << names.at(live) << ");\n";
  }
  output << "  return absl::OkStatus();\n}\n\n";
}

std::string EmitModel(const ModelDescription& model) {
  std::ostringstream output;
  output << "// Generated from XLS scheduled_proc IR.\n"
         << "#include \"xls/experimental/stage_gfsim/model.h\"\n"
         << "#include \"xls/common/status/status_macros.h\"\n"
         << "#include \"xls/ir/bits_ops.h\"\n\n"
         << "namespace xls::stage_gfsim {\nnamespace {\n\n";

  NameUniquer unique("_");
  std::vector<std::vector<std::string>> functions;
  for (const ProcDescription& proc : model.procs) {
    auto& stages = functions.emplace_back();
    for (size_t i = 0; i < proc.stages.size(); ++i) {
      stages.push_back(CppName(unique, "Eval_",
                               absl::StrCat(proc.proc->name(), "_Stage", i)));
      EmitStage(output, proc.stages[i], stages.back());
    }
  }
  output << "const ModelSpec kModel{\n    .procs = {\n";
  for (size_t i = 0; i < model.procs.size(); ++i) {
    output << "        {\n          .name = "
           << Quote(model.procs[i].proc->name())
           << ",\n          .stages = {\n";
    for (const std::string& function : functions[i]) {
      output << "              " << function << ",\n";
    }
    output << "          },\n        },\n";
  }
  output << "    },\n    .channels = {\n";
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
  output << "    },\n    .instances = {\n";
  for (const InstanceSpec& instance : model.instances) {
    output << "        {\n          .name = " << Quote(instance.name)
           << ",\n          .proc = " << Quote(instance.proc)
           << ",\n          .bindings = {\n";
    for (const Binding& binding : instance.bindings) {
      output << "              {.node = " << Quote(binding.node)
             << ", .channel = " << Quote(binding.channel) << "},\n";
    }
    output << "          },\n        },\n";
  }
  output << "    },\n};\n}  // namespace\n}  // namespace xls::stage_gfsim\n\n"
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
