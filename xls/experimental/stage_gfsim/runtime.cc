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

#include <chrono>
#include <cstddef>
#include <cstdint>
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
ABSL_FLAG(bool, trace, true, "Print the channel transfer trace.");
ABSL_FLAG(bool, simulation_time, false,
          "Report the simulation loop time in ns.");

namespace xls::stage_gfsim {

void TraceTransfer(gfsim::SimModule& module, std::string_view kind,
                   const gfsim::SimObj& queue, const Value& value) {
  std::cout << module.GetSim()->Cycles() << '\t' << kind << '\t'
            << module.Name() << '\t' << queue.Name() << '\t' << value.ToString()
            << '\n';
}

ProcRuntime::ProcRuntime(gfsim::SimModule& module, Proc& ir,
                         std::size_t stage_count, bool interpret)
    : module_(module), ir_(ir), stats_(stage_count) {
  if (interpret) {
    for (Node* node : ir.nodes()) {
      nodes_.emplace(node->GetName(), node);
    }
  }
}

void ProcRuntime::Reset() { stats_.assign(stats_.size(), {}); }

const Value& ProcRuntime::InitialState(std::string_view name) const {
  return ir_.GetStateElementByName(name).value()->initial_value();
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

std::optional<Value> StageContext::Receive(
    const TypedChannel<ValueCodec>& channel, std::string_view node,
    bool enabled) {
  if (!enabled) {
    return ZeroOfType(proc_.nodes_.at(std::string(node))->GetType());
  }
  if (const Value* data = ReceiveData(channel)) {
    return Value::Tuple({Value::Token(), *data});
  }
  return std::nullopt;
}

bool StageContext::ReceiveReady(const SimChannel& channel) {
  if (!channel.readable) {
    ++proc_.stats_[index_].input;
    return false;
  }
  receives_.push_back(&channel);
  return true;
}

bool StageContext::ProposeSend(const SimChannel& channel, const void* data,
                               bool enabled) {
  if (!enabled) {
    return true;
  }
  if (!channel.writable) {
    ++proc_.stats_[index_].output;
    return false;
  }
  send_ = SendEffect{&channel, data};
  return true;
}

bool StageContext::StateReady(bool available) {
  if (!available) {
    ++proc_.stats_[index_].state;
  }
  return available;
}

void StageContext::Finish() {
  gfsim::SimModule& module = proc_.module_;
  for (const SimChannel* channel : receives_) {
    channel->Consume(module, "receive");
  }
  if (send_) {
    send_->channel->Produce(module, send_->data);
  }
  ++proc_.stats_[index_].fired;
  module.GetSim()->MarkProgress();
}

namespace {

using Stimuli = std::unordered_map<std::string, std::vector<Value>>;

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

class Network final : public ChannelOwner {
 public:
  Network(gfsim::SimSystem& system, const ModelSpec& model, Package& package,
          const Stimuli& inputs, bool trace)
      : ChannelOwner("stage_gfsim", &system),
        model_(model),
        package_(package),
        trace_(trace),
        stall_output_(absl::GetFlag(FLAGS_stall_output)),
        stall_first_(absl::GetFlag(FLAGS_stall_first)),
        stall_last_(absl::GetFlag(FLAGS_stall_last)) {
    for (const ChannelSpec& spec : model_.channels) {
      auto channel = spec.make_channel();
      if (spec.port == Port::kInput) {
        for (const Value& input : inputs.at(spec.name)) {
          channel->AddInput(input);
        }
      }
      channels_.push_back(std::move(channel));
    }
  }

  void Build() override {
    SetObjectCapacity(model_.channels.size() + model_.proc_count);
    Channels channels_by_name;
    for (size_t i = 0; i < model_.channels.size(); ++i) {
      const ChannelSpec& spec = model_.channels[i];
      channels_[i]->Build(*this, spec, trace_);
      channels_by_name.emplace(spec.name, channels_[i].get());
    }
    absl::Status status = model_.build_procs(*this, package_, channels_by_name);
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }
    BuildInnerQueues();
    BuildChildren();
  }

  void Reset() override {
    for (const auto& channel : channels_) {
      channel->ResetInputs();
    }
    delivered_ = 0;
    first_source_.reset();
    last_sink_.reset();
    ResetInnerQueues();
    ResetChildren();
  }

  void Work() override {
    // Take all FIFO admission decisions from the same cycle boundary.
    for (const auto& channel : channels_) {
      channel->Sample();
    }
    WorkChildren();

    const uint64_t cycle = GetSim()->Cycles();
    for (size_t i = 0; i < channels_.size(); ++i) {
      const ChannelSpec& spec = model_.channels[i];
      SimChannel& channel = *channels_[i];
      if (spec.port == Port::kInput && channel.Source(*this)) {
        if (!first_source_) {
          first_source_ = cycle;
        }
        GetSim()->MarkProgress();
      }
      if (spec.port == Port::kOutput && channel.readable &&
          !(spec.name == stall_output_ && cycle >= stall_first_ &&
            cycle <= stall_last_)) {
        channel.Consume(*this, "sink");
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
  std::vector<std::unique_ptr<SimChannel>> channels_;
  bool trace_;
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
  const bool trace = absl::GetFlag(FLAGS_trace);
  system.AddModule(
      std::make_shared<Network>(system, model, *package, inputs, trace));
  system.Build();
  if (trace) {
    std::cout << "cycle\tevent\tinstance\tchannel\tvalue\n";
  }
  const auto start = std::chrono::steady_clock::now();
  for (int64_t cycle = 0; cycle < cycles; ++cycle) {
    system.Step();
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (absl::GetFlag(FLAGS_simulation_time)) {
    std::cerr
        << "simulation_ns="
        << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
        << '\n';
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
