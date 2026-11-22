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
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SimModule.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/common/exit_status.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/common/status/status_macros.h"
#include "xls/experimental/stage_gfsim/model.h"
#include "xls/experimental/stage_gfsim/pipeline.h"
#include "xls/interpreter/ir_interpreter.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/package.h"
#include "xls/ir/proc.h"
#include "xls/ir/state_element.h"
#include "xls/ir/value.h"
#include "xls/ir/value_utils.h"

ABSL_FLAG(std::string, inputs, "",
          "Input file with one channel and typed value per line.");
ABSL_FLAG(int64_t, cycles, 64, "Number of cycles to simulate.");
ABSL_FLAG(int64_t, clock_period_ps, 0, "Clock period for time estimates.");
ABSL_FLAG(std::string, stall_output, "", "Output channel to stop draining.");
ABSL_FLAG(int64_t, stall_first, 0, "First cycle of the output stall.");
ABSL_FLAG(int64_t, stall_last, 0,
          "Last cycle of the output stall (inclusive).");

namespace xls::stage_gfsim {
namespace {

using Queue = gfsim::SimQueue<Value>;
using Values = std::unordered_map<std::string_view, Value>;

struct Channel {
  Queue* queue = nullptr;
  bool readable = false;
  bool writable = false;
};

using ChannelBindings = std::unordered_map<std::string_view, Channel*>;

struct ProcPlan {
  Proc* proc;
  std::vector<StageFunction> stages;
  std::unordered_map<std::string, Node*> nodes;
  std::unordered_set<const StateElement*> feedback;
};

using ProcPlans = std::unordered_map<std::string_view, ProcPlan>;

absl::StatusOr<ProcPlan> ResolveProc(Package& package, const ProcSpec& spec) {
  XLS_ASSIGN_OR_RETURN(Proc * proc, package.GetProc(spec.name));
  ProcPlan plan{
      .proc = proc, .stages = spec.stages, .nodes = {}, .feedback = {}};
  for (Node* node : proc->nodes()) {
    plan.nodes.emplace(node->GetName(), node);
  }
  for (Next* write : proc->next_values()) {
    XLS_ASSIGN_OR_RETURN(int64_t write_stage, proc->GetStageIndex(write));
    for (StateRead* read :
         proc->GetStateReadsByStateElement(write->state_element())) {
      XLS_ASSIGN_OR_RETURN(int64_t read_stage, proc->GetStageIndex(read));
      if (read_stage < write_stage) {
        plan.feedback.insert(write->state_element());
      }
    }
  }
  return plan;
}

enum class Stall { kNone, kInput, kOutput, kState };

struct StageStats {
  uint64_t fired = 0;
  uint64_t input = 0;
  uint64_t output = 0;
  uint64_t state = 0;
};

struct State {
  Value value;
  bool available = true;
};

struct Effects {
  struct Send {
    Channel* channel;
    Value value;
  };
  struct Write {
    const StateElement* element;
    std::optional<Value> value;
  };
  std::vector<Channel*> receives;
  std::optional<Send> send;
  std::vector<const StateElement*> reads;
  std::vector<Write> writes;
};

using StateValues = std::unordered_map<const StateElement*, State>;

}  // namespace

struct StageContext::Data {
  const ProcPlan& plan;
  const Values& inputs;
  const ChannelBindings& channels;
  const StateValues& state;
  Values outputs;
  Effects effects;
  Stall stall = Stall::kNone;
};

const Value& StageContext::Input(std::string_view name) const {
  return data_.inputs.at(name);
}

const Value& StageContext::Literal(std::string_view name) const {
  return data_.plan.nodes.at(std::string(name))->As<xls::Literal>()->value();
}

absl::StatusOr<Value> StageContext::Interpret(
    std::string_view name, absl::Span<const Value> operands) const {
  return InterpretNode(data_.plan.nodes.at(std::string(name)), operands);
}

std::optional<Value> StageContext::Receive(std::string_view node,
                                           bool enabled) {
  if (!enabled) {
    return ZeroOfType(data_.plan.nodes.at(std::string(node))->GetType());
  }
  Channel* channel = data_.channels.at(node);
  if (!channel->readable) {
    data_.stall = Stall::kInput;
    return std::nullopt;
  }
  data_.effects.receives.push_back(channel);
  return Value::Tuple({Value::Token(), channel->queue->Peek()});
}

bool StageContext::Send(std::string_view node, const Value& value,
                        bool enabled) {
  if (!enabled) {
    return true;
  }
  Channel* channel = data_.channels.at(node);
  if (!channel->writable) {
    data_.stall = Stall::kOutput;
    return false;
  }
  data_.effects.send = Effects::Send{channel, value};
  return true;
}

const Value* StageContext::ReadState(std::string_view element) {
  const StateElement* key =
      data_.plan.proc->GetStateElementByName(element).value();
  const State& state = data_.state.at(key);
  if (!state.available) {
    data_.stall = Stall::kState;
    return nullptr;
  }
  if (data_.plan.feedback.contains(key)) {
    data_.effects.reads.push_back(key);
  }
  return &state.value;
}

void StageContext::WriteState(std::string_view element, const Value& value,
                              bool enabled) {
  const StateElement* key =
      data_.plan.proc->GetStateElementByName(element).value();
  data_.effects.writes.push_back(
      {key, enabled ? std::optional<Value>(value) : std::nullopt});
}

void StageContext::Forward(std::string_view name, Value value) {
  data_.outputs.emplace(name, std::move(value));
}

namespace {

void Transfer(uint64_t cycle, std::string_view kind, std::string_view instance,
              const Queue& queue, const Value& value) {
  std::cout << cycle << '\t' << kind << '\t' << instance << '\t' << queue.Name()
            << '\t' << value.ToString() << '\n';
}

class ProcModule final : public gfsim::SimModule {
 public:
  ProcModule(const InstanceSpec& spec, const ProcPlan& plan,
             const ChannelBindings& channels)
      : SimModule(spec.name), plan_(plan), pipeline_(plan.stages.size()) {
    for (const Binding& binding : spec.bindings) {
      Channel* channel = channels.at(binding.channel);
      Node* operation = plan.nodes.at(binding.node);
      channel_for_node_.emplace(binding.node, channel);
      if (operation->Is<Send>()) {
        BindOutput(channel->queue);
      } else {
        BindInput(channel->queue);
      }
    }
  }

  void Build() override { SetObjectCapacity(0); }

  void Reset() override {
    state_.clear();
    for (StateElement* element : plan_.proc->StateElements()) {
      state_.emplace(element, State{element->initial_value()});
    }
    pipeline_.Reset();
    stats_.assign(plan_.stages.size(), {});
  }

  void Work() override {
    pipeline_.BeginCycle();
    next_state_ = state_;
    for (size_t i = plan_.stages.size(); i-- > 0;) {
      StageStats& stats = stats_[i];
      if (!pipeline_.InputValid(i)) {
        ++stats.input;
        continue;
      }
      if (!pipeline_.OutputReady(i)) {
        ++stats.output;
        continue;
      }

      const Values empty;
      StageContext::Data data{
          .plan = plan_,
          .inputs = i == 0 ? empty : pipeline_.Input(i),
          .channels = channel_for_node_,
          .state = state_,
          .outputs = {},
          .effects = {},
      };
      StageContext stage(data);
      absl::Status status = plan_.stages[i](stage);
      if (!status.ok()) {
        throw std::runtime_error(Name() + " stage " + std::to_string(i) + ": " +
                                 status.ToString());
      }
      switch (data.stall) {
        case Stall::kInput:
          ++stats.input;
          continue;
        case Stall::kOutput:
          ++stats.output;
          continue;
        case Stall::kState:
          ++stats.state;
          continue;
        case Stall::kNone:
          break;
      }

      Commit(data.effects);
      pipeline_.Advance(i, std::move(data.outputs));
      ++stats.fired;
      GetSim()->MarkProgress();
    }
  }

  void Xfer() override {
    pipeline_.EndCycle();
    state_.swap(next_state_);
  }

  void ReportStat() override {
    for (size_t i = 0; i < stats_.size(); ++i) {
      const StageStats& stage = stats_[i];
      const std::string name = Name() + ".stage_" + std::to_string(i);
      Report().ReportVal(name + ".fire_cycles", stage.fired);
      Report().ReportVal(name + ".input_stall_cycles", stage.input);
      Report().ReportVal(name + ".output_stall_cycles", stage.output);
      Report().ReportVal(name + ".state_stall_cycles", stage.state);
    }
  }

 private:
  void Commit(const Effects& effects) {
    const uint64_t cycle = GetSim()->Cycles();
    for (Channel* channel : effects.receives) {
      auto value = channel->queue->Read();
      Transfer(cycle, "receive", Name(), *channel->queue, *value);
    }
    if (effects.send) {
      const auto& [channel, value] = *effects.send;
      channel->queue->Write(std::make_shared<Value>(value));
      Transfer(cycle, "send", Name(), *channel->queue, value);
    }
    for (const StateElement* element : effects.reads) {
      next_state_.at(element).available = false;
    }
    for (const auto& [element, value] : effects.writes) {
      State& state = next_state_.at(element);
      if (value) {
        state.value = *value;
      }
      state.available = true;
    }
  }

  const ProcPlan& plan_;
  ChannelBindings channel_for_node_;
  Pipeline<Values> pipeline_;
  StateValues state_;
  StateValues next_state_;
  std::vector<StageStats> stats_;
};

using Stimuli = std::unordered_map<std::string, std::deque<Value>>;

absl::StatusOr<Stimuli> ReadInputs(const std::string& path,
                                   const ModelSpec& model) {
  Stimuli inputs;
  for (const ChannelSpec& channel : model.channels) {
    if (channel.port == Port::kInput) {
      inputs.try_emplace(channel.name);
    }
  }
  if (path.empty()) {
    return inputs;
  }
  std::ifstream file(path);
  if (!file) {
    return absl::NotFoundError("Cannot read " + path);
  }
  for (std::string line; std::getline(file, line);) {
    std::istringstream row(line);
    std::string name;
    if (!(row >> name) || name.starts_with('#')) {
      continue;
    }
    auto it = inputs.find(name);
    if (it == inputs.end()) {
      return absl::InvalidArgumentError("Unknown input channel: " + name);
    }
    std::string value;
    std::getline(row >> std::ws, value);
    XLS_ASSIGN_OR_RETURN(Value parsed, Parser::ParseTypedValue(value));
    it->second.push_back(std::move(parsed));
  }
  return inputs;
}

class Network final : public gfsim::SimModule {
 public:
  Network(gfsim::SimSystem& system, const ModelSpec& model,
          const ProcPlans& plans, Stimuli inputs)
      : SimModule("stage_gfsim", &system),
        model_(model),
        plans_(plans),
        stimuli_(std::move(inputs)),
        channels_(model.channels.size()),
        stall_output_(absl::GetFlag(FLAGS_stall_output)),
        stall_first_(absl::GetFlag(FLAGS_stall_first)),
        stall_last_(absl::GetFlag(FLAGS_stall_last)) {}

  void Build() override {
    SetObjectCapacity(model_.channels.size() + model_.instances.size());
    ChannelBindings channels_by_name;
    for (size_t i = 0; i < model_.channels.size(); ++i) {
      const ChannelSpec& spec = model_.channels[i];
      auto queue = std::make_shared<Queue>(spec.name);
      queue->InitMaxSize(spec.depth);
      queue->Setlatency(1);
      channels_[i].queue = AddInnerQueue(queue);
      channels_by_name.emplace(spec.name, &channels_[i]);
      if (spec.port == Port::kInput) {
        BindOutput(queue.get());
      } else if (spec.port == Port::kOutput) {
        BindInput(queue.get());
      }
    }
    for (const InstanceSpec& spec : model_.instances) {
      AddSubmodule(std::make_shared<ProcModule>(spec, plans_.at(spec.proc),
                                                channels_by_name));
    }
    BuildInnerQueues();
    BuildChildren();
  }

  void Reset() override {
    pending_ = stimuli_;
    delivered_ = 0;
    first_source_.reset();
    last_sink_.reset();
    ResetInnerQueues();
    ResetChildren();
  }

  void Work() override {
    // Take all FIFO admission decisions from the same cycle boundary.
    for (Channel& channel : channels_) {
      channel.readable = !channel.queue->Empty();
      channel.writable = !channel.queue->Full();
    }
    WorkChildren();

    const uint64_t cycle = GetSim()->Cycles();
    for (size_t i = 0; i < channels_.size(); ++i) {
      const ChannelSpec& spec = model_.channels[i];
      Channel& channel = channels_[i];
      if (spec.port == Port::kInput && channel.writable) {
        auto& pending = pending_.at(spec.name);
        if (!pending.empty()) {
          auto value = std::make_shared<Value>(std::move(pending.front()));
          Transfer(cycle, "source", Name(), *channel.queue, *value);
          channel.queue->Write(std::move(value));
          if (!first_source_) {
            first_source_ = cycle;
          }
          pending.pop_front();
          GetSim()->MarkProgress();
        }
      }
      if (spec.port == Port::kOutput && channel.readable &&
          !(spec.name == stall_output_ && cycle >= stall_first_ &&
            cycle <= stall_last_)) {
        auto value = channel.queue->Read();
        Transfer(cycle, "sink", Name(), *channel.queue, *value);
        last_sink_ = cycle;
        ++delivered_;
        GetSim()->MarkProgress();
      }
    }
  }

  void Xfer() override {
    AdvanceInnerQueues();
    XferChildren();
  }

  void ReportStat() override {
    const uint64_t cycles = GetSim()->Cycles();
    Report().ReportVal("cycles", cycles);
    Report().ReportVal("delivered_outputs", delivered_);
    Report().ReportAvg("outputs_per_cycle", delivered_, cycles);
    const int64_t period = absl::GetFlag(FLAGS_clock_period_ps);
    if (period > 0) {
      Report().ReportVal("estimated_time_ps",
                         static_cast<double>(cycles) * period, 0);
    }
    if (first_source_ && last_sink_ && *first_source_ <= *last_sink_) {
      const uint64_t workload_cycles = *last_sink_ - *first_source_ + 1;
      Report().ReportVal("workload_cycles", workload_cycles);
      if (period > 0) {
        Report().ReportVal("estimated_workload_time_ps",
                           static_cast<double>(workload_cycles) * period, 0);
      }
    }
    ReportChildren();
  }

 private:
  const ModelSpec& model_;
  const ProcPlans& plans_;
  const Stimuli stimuli_;
  Stimuli pending_;
  std::vector<Channel> channels_;
  std::string stall_output_;
  uint64_t stall_first_;
  uint64_t stall_last_;
  uint64_t delivered_ = 0;
  std::optional<uint64_t> first_source_;
  std::optional<uint64_t> last_sink_;
};

absl::Status Simulate(std::string_view path, const ModelSpec& model) {
  const int64_t cycles = absl::GetFlag(FLAGS_cycles);
  if (cycles < 1 || absl::GetFlag(FLAGS_stall_first) < 0 ||
      absl::GetFlag(FLAGS_stall_last) < absl::GetFlag(FLAGS_stall_first) ||
      absl::GetFlag(FLAGS_clock_period_ps) < 0) {
    return absl::InvalidArgumentError("Invalid cycle, stall, or clock setting");
  }
  XLS_ASSIGN_OR_RETURN(std::string text, GetFileContents(path));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Package> package,
                       Parser::ParsePackage(text, path));
  ProcPlans plans;
  plans.reserve(model.procs.size());
  for (const ProcSpec& spec : model.procs) {
    XLS_ASSIGN_OR_RETURN(ProcPlan plan, ResolveProc(*package, spec));
    plans.emplace(spec.name, std::move(plan));
  }
  XLS_ASSIGN_OR_RETURN(Stimuli inputs,
                       ReadInputs(absl::GetFlag(FLAGS_inputs), model));
  gfsim::SimSystem system;
  system.SetDeadlockThreshold(0);
  system.GetReporter().SetOutput(std::cerr);
  system.AddModule(
      std::make_shared<Network>(system, model, plans, std::move(inputs)));
  system.Build();
  std::cout << "cycle\tevent\tinstance\tchannel\tvalue\n";
  for (int64_t cycle = 0; cycle < cycles; ++cycle) {
    system.Step();
  }
  system.ReportStat();
  return absl::OkStatus();
}

}  // namespace

int RunModel(int argc, char** argv, const ModelSpec& model) {
  constexpr std::string_view kUsage =
      "model [--inputs=FILE] [--cycles=N] [--clock_period_ps=N] scheduled.ir";
  auto args = InitXls(kUsage, argc, argv);
  if (args.size() != 1) {
    return ExitStatus(absl::InvalidArgumentError(kUsage));
  }
  try {
    return ExitStatus(Simulate(args.front(), model));
  } catch (const std::exception& error) {
    return ExitStatus(absl::InternalError(error.what()));
  }
}

}  // namespace xls::stage_gfsim
