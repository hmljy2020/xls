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

#ifndef XLS_EXPERIMENTAL_STAGE_GFSIM_PIPELINE_H_
#define XLS_EXPERIMENTAL_STAGE_GFSIM_PIPELINE_H_

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace xls::stage_gfsim {

template <typename Payload>
class Pipeline {
 public:
  explicit Pipeline(std::size_t stages) : inputs_(stages - 1) {}

  void Reset() { inputs_.assign(inputs_.size(), std::nullopt); }
  void BeginCycle() { next_inputs_ = inputs_; }

  bool InputValid(std::size_t stage) const {
    return stage == 0 || inputs_[stage - 1].has_value();
  }
  const Payload& Input(std::size_t stage) const { return *inputs_[stage - 1]; }

  // Visit stages from last to first. The next slot becomes available as soon
  // as the downstream stage claims its old input for this cycle.
  bool OutputReady(std::size_t stage) const {
    return stage == inputs_.size() || !next_inputs_[stage].has_value();
  }

  void Advance(std::size_t stage, Payload payload) {
    if (stage > 0) {
      next_inputs_[stage - 1].reset();
    }
    if (stage < inputs_.size()) {
      next_inputs_[stage] = std::move(payload);
    }
  }

  void EndCycle() { inputs_.swap(next_inputs_); }

 private:
  std::vector<std::optional<Payload>> inputs_;
  std::vector<std::optional<Payload>> next_inputs_;
};

}  // namespace xls::stage_gfsim

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_PIPELINE_H_
