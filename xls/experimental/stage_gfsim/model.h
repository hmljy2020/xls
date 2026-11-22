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

#ifndef XLS_EXPERIMENTAL_STAGE_GFSIM_MODEL_H_
#define XLS_EXPERIMENTAL_STAGE_GFSIM_MODEL_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/statusor.h"
#include "xls/ir/value.h"

namespace gfsim {
class SimSystem;
}  // namespace gfsim

namespace xls {
class Package;

namespace stage_gfsim {

class NetworkRuntime;

using Stimuli = std::unordered_map<std::string, std::vector<Value>>;

enum class Port { kInternal, kInput, kOutput };

struct ChannelSpec {
  std::string name;
  Port port;
  int depth;
};

struct RunOptions {
  bool trace;
  std::string stall_output;
  uint64_t stall_first;
  uint64_t stall_last;
};

struct ModelSpec {
  std::span<const ChannelSpec> channels;
  absl::StatusOr<std::shared_ptr<NetworkRuntime>> (*build_network)(
      gfsim::SimSystem&, Package&, const Stimuli&, const RunOptions&);
};

// Simulates a model with the scheduled IR from which it was generated.
int RunModel(int argc, char** argv, const ModelSpec& model);

}  // namespace stage_gfsim
}  // namespace xls

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_MODEL_H_
