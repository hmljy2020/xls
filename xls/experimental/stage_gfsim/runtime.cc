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
#include "xls/experimental/stage_gfsim/proc_runtime.h"
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

void Transfer(uint64_t cycle, std::string_view kind, std::string_view instance,
              const Queue& queue, const Value& value) {
  std::cout << cycle << '\t' << kind << '\t' << instance << '\t' << queue.Name()
            << '\t' << value.ToString() << '\n';
}

}  // namespace

ProcRuntime::ProcRuntime(gfsim::SimModule& module, Proc& ir,
                         std::size_t stage_count)
    : module_(module), ir_(ir), stats_(stage_count) {
  for (Node* node : ir.nodes()) {
    nodes_.emplace(node->GetName(), node);
  }
  for (Next* write : ir.next_values()) {
    const int64_t write_stage = ir.GetStageIndex(write).value();
    for (StateRead* read :
         ir.GetStateReadsByStateElement(write->state_element())) {
      if (ir.GetStageIndex(read).value() < write_stage) {
        feedback_.insert(write->state_element());
      }
    }
  }
}

void ProcRuntime::Reset() {
  state_.clear();
  for (StateElement* element : ir_.StateElements()) {
    state_.emplace(element, State{element->initial_value()});
  }
  stats_.assign(stats_.size(), {});
}

bool ProcRuntime::StageReady(std::size_t index, bool input_valid,
                             bool output_ready) {
  if (!input_valid) {
    ++stats_[index].input;
    return false;
  }
  if (!output_ready) {
    ++stats_[index].output;
    return false;
  }
  return true;
}

void ProcRuntime::ReportStat(gfsim::Reporter& report) const {
  for (std::size_t i = 0; i < stats_.size(); ++i) {
    const StageStats& stage = stats_[i];
    const std::string name = module_.Name() + ".stage_" + std::to_string(i);
    report.ReportVal(name + ".fire_cycles", stage.fired);
    report.ReportVal(name + ".input_stall_cycles", stage.input);
    report.ReportVal(name + ".output_stall_cycles", stage.output);
    report.ReportVal(name + ".state_stall_cycles", stage.state);
  }
}

const Value& StageContext::Input(std::string_view name) const {
  return pipeline_.Input(index_).at(name);
}

const Value& StageContext::Literal(std::string_view name) const {
  return proc_.nodes_.at(std::string(name))->As<xls::Literal>()->value();
}

Value StageContext::Interpret(std::string_view name,
                              absl::Span<const Value> operands) const {
  absl::StatusOr<Value> result =
      InterpretNode(proc_.nodes_.at(std::string(name)), operands);
  if (!result.ok()) {
    throw std::runtime_error(proc_.module_.Name() + " stage " +
                             std::to_string(index_) + ": " +
                             result.status().ToString());
  }
  return std::move(*result);
}

std::optional<Value> StageContext::Receive(const SimChannel& channel,
                                           std::string_view node,
                                           bool enabled) {
  if (!enabled) {
    return ZeroOfType(proc_.nodes_.at(std::string(node))->GetType());
  }
  if (!channel.readable) {
    ++proc_.stats_[index_].input;
    return std::nullopt;
  }
  receives_.push_back(&channel);
  return Value::Tuple({Value::Token(), channel.queue->Peek()});
}

bool StageContext::Send(const SimChannel& channel, const Value& value,
                        bool enabled) {
  if (!enabled) {
    return true;
  }
  if (!channel.writable) {
    ++proc_.stats_[index_].output;
    return false;
  }
  send_ = SendEffect{&channel, value};
  return true;
}

absl::optional_ref<const Value> StageContext::ReadState(
    std::string_view element) {
  const StateElement* key = proc_.ir_.GetStateElementByName(element).value();
  const ProcRuntime::State& state = proc_.state_.at(key);
  if (!state.available) {
    ++proc_.stats_[index_].state;
    return std::nullopt;
  }
  if (proc_.feedback_.contains(key)) {
    reads_.push_back(key);
  }
  return state.value;
}

void StageContext::WriteState(std::string_view element, const Value& value,
                              bool enabled) {
  const StateElement* key = proc_.ir_.GetStateElementByName(element).value();
  writes_.push_back(
      {key, enabled ? std::optional<Value>(value) : std::nullopt});
}

void StageContext::Forward(std::string_view name, Value value) {
  outputs_.emplace(name, std::move(value));
}

StageValues StageContext::Commit() {
  gfsim::SimModule& module = proc_.module_;
  const uint64_t cycle = module.GetSim()->Cycles();
  for (const SimChannel* channel : receives_) {
    auto value = channel->queue->Read();
    Transfer(cycle, "receive", module.Name(), *channel->queue, *value);
  }
  if (send_) {
    const auto& [channel, value] = *send_;
    channel->queue->Write(std::make_shared<Value>(value));
    Transfer(cycle, "send", module.Name(), *channel->queue, value);
  }
  for (const StateElement* element : reads_) {
    proc_.next_state_.at(element).available = false;
  }
  for (const auto& [element, value] : writes_) {
    ProcRuntime::State& state = proc_.next_state_.at(element);
    if (value) {
      state.value = *value;
    }
    state.available = true;
  }
  ++proc_.stats_[index_].fired;
  module.GetSim()->MarkProgress();
  return std::move(outputs_);
}

namespace {

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
  Network(gfsim::SimSystem& system, const ModelSpec& model, Package& package,
          Stimuli inputs)
      : SimModule("stage_gfsim", &system),
        model_(model),
        package_(package),
        stimuli_(std::move(inputs)),
        channels_(model.channels.size()),
        stall_output_(absl::GetFlag(FLAGS_stall_output)),
        stall_first_(absl::GetFlag(FLAGS_stall_first)),
        stall_last_(absl::GetFlag(FLAGS_stall_last)) {}

  void Build() override {
    SetObjectCapacity(model_.channels.size() + model_.proc_count);
    Channels channels_by_name;
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
    absl::Status status = model_.build_procs(*this, package_, channels_by_name);
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
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
    for (SimChannel& channel : channels_) {
      channel.readable = !channel.queue->Empty();
      channel.writable = !channel.queue->Full();
    }
    WorkChildren();

    const uint64_t cycle = GetSim()->Cycles();
    for (size_t i = 0; i < channels_.size(); ++i) {
      const ChannelSpec& spec = model_.channels[i];
      SimChannel& channel = channels_[i];
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
  Package& package_;
  const Stimuli stimuli_;
  Stimuli pending_;
  std::vector<SimChannel> channels_;
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
  XLS_ASSIGN_OR_RETURN(Stimuli inputs,
                       ReadInputs(absl::GetFlag(FLAGS_inputs), model));
  gfsim::SimSystem system;
  system.SetDeadlockThreshold(0);
  system.GetReporter().SetOutput(std::cerr);
  system.AddModule(
      std::make_shared<Network>(system, model, *package, std::move(inputs)));
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
