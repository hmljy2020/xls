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
#include <vector>

#include "SimModule.h"
#include "absl/types/span.h"
#include "xls/ir/value.h"

namespace xls {
class Node;
class Proc;

namespace stage_gfsim {

struct SimChannel {
  gfsim::SimQueue<Value>* queue = nullptr;
  bool readable = false;
  bool writable = false;
};

// IR lookups and counters for one generated proc module.
class ProcRuntime {
 public:
  ProcRuntime(gfsim::SimModule& module, Proc& ir, std::size_t stage_count);
  void Reset();
  const Value& InitialState(std::string_view name) const;
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

  gfsim::SimModule& module_;
  Proc& ir_;
  std::unordered_map<std::string, Node*> nodes_;
  std::vector<StageStats> stats_;
};

// Collects FIFO transfers until the stage can complete all its operations.
class StageContext {
 public:
  StageContext(ProcRuntime& proc, std::size_t index)
      : proc_(proc), index_(index) {}

  const Value& Literal(std::string_view name) const;
  Value Interpret(std::string_view name,
                  absl::Span<const Value> operands) const;

  std::optional<Value> Receive(const SimChannel& channel, std::string_view node,
                               bool enabled = true);
  bool Send(const SimChannel& channel, const Value& value, bool enabled = true);
  bool StateReady(bool available);
  void Finish();

 private:
  struct SendEffect {
    const SimChannel* channel;
    Value value;
  };

  ProcRuntime& proc_;
  std::size_t index_;
  std::vector<const SimChannel*> receives_;
  std::optional<SendEffect> send_;
};

}  // namespace stage_gfsim
}  // namespace xls

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_PROC_RUNTIME_H_
