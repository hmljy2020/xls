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

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"

namespace gfsim {
class SimModule;
}

namespace xls {
class Package;

namespace stage_gfsim {

struct SimChannel;
using Channels = std::unordered_map<std::string_view, SimChannel*>;

enum class Port { kInternal, kInput, kOutput };

struct ChannelSpec {
  std::string name;
  Port port;
  int depth;
};

struct ModelSpec {
  std::vector<ChannelSpec> channels;
  std::size_t proc_count;
  absl::Status (*build_procs)(gfsim::SimModule&, Package&, const Channels&);
};

// Simulates a model with the scheduled IR from which it was generated.
int RunModel(int argc, char** argv, const ModelSpec& model);

}  // namespace stage_gfsim
}  // namespace xls

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_MODEL_H_
