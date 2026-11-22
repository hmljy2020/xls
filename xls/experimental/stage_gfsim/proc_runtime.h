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

#ifndef XLS_EXPERIMENTAL_STAGE_GFSIM_PROC_RUNTIME_H_
#define XLS_EXPERIMENTAL_STAGE_GFSIM_PROC_RUNTIME_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SimModule.h"
#include "absl/types/optional_ref.h"
#include "absl/types/span.h"
#include "xls/experimental/stage_gfsim/pipeline.h"
#include "xls/ir/value.h"

namespace xls {
class Node;
class Proc;
class StateElement;

namespace stage_gfsim {

struct SimChannel {
  gfsim::SimQueue<Value>* queue = nullptr;
  bool readable = false;
  bool writable = false;
};

using StageValues = std::unordered_map<std::string_view, Value>;

// Common state and counters for one generated proc module.
class ProcRuntime {
 public:
  ProcRuntime(gfsim::SimModule& module, Proc& ir, std::size_t stage_count);
  void Reset();
  void BeginCycle() { next_state_ = state_; }
  void PublishState() { state_.swap(next_state_); }
  bool StageReady(std::size_t index, bool input_valid, bool output_ready);
  void ReportStat(gfsim::Reporter& report) const;

 private:
  friend class StageContext;

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

  gfsim::SimModule& module_;
  Proc& ir_;
  std::unordered_map<std::string, Node*> nodes_;
  std::unordered_set<const StateElement*> feedback_;
  std::unordered_map<const StateElement*, State> state_;
  std::unordered_map<const StateElement*, State> next_state_;
  std::vector<StageStats> stats_;
};

// Collects a stage's effects until it can complete all its operations.
class StageContext {
 public:
  StageContext(ProcRuntime& proc, const Pipeline<StageValues>& pipeline,
               std::size_t index)
      : proc_(proc), pipeline_(pipeline), index_(index) {}

  const Value& Input(std::string_view name) const;
  const Value& Literal(std::string_view name) const;
  Value Interpret(std::string_view name,
                  absl::Span<const Value> operands) const;

  std::optional<Value> Receive(const SimChannel& channel, std::string_view node,
                               bool enabled = true);
  bool Send(const SimChannel& channel, const Value& value, bool enabled = true);
  absl::optional_ref<const Value> ReadState(std::string_view element);
  void WriteState(std::string_view element, const Value& value,
                  bool enabled = true);
  void Forward(std::string_view name, Value value);

  StageValues Commit();

 private:
  struct SendEffect {
    const SimChannel* channel;
    Value value;
  };
  struct StateWrite {
    const StateElement* element;
    std::optional<Value> value;
  };

  ProcRuntime& proc_;
  const Pipeline<StageValues>& pipeline_;
  std::size_t index_;
  StageValues outputs_;
  std::vector<const SimChannel*> receives_;
  std::optional<SendEffect> send_;
  std::vector<const StateElement*> reads_;
  std::vector<StateWrite> writes_;
};

}  // namespace stage_gfsim
}  // namespace xls

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_PROC_RUNTIME_H_
