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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/ir/value.h"

namespace xls::stage_gfsim {

// A stage sees the current cycle's inputs. Side effects are recorded here and
// committed by the runtime when the stage advances.
class StageContext {
 public:
  struct Data;
  explicit StageContext(Data& data) : data_(data) {}

  const Value& Input(std::string_view name) const;
  const Value& Literal(std::string_view name) const;
  absl::StatusOr<Value> Interpret(std::string_view name,
                                  absl::Span<const Value> operands) const;

  std::optional<Value> Receive(std::string_view node, bool enabled = true);
  bool Send(std::string_view node, const Value& value, bool enabled = true);
  const Value* ReadState(std::string_view element);
  void WriteState(std::string_view element, const Value& value,
                  bool enabled = true);
  void Forward(std::string_view name, Value value);

 private:
  Data& data_;
};

using StageFunction = absl::Status (*)(StageContext&);

struct ProcSpec {
  std::string name;
  std::vector<StageFunction> stages;
};

enum class Port { kInternal, kInput, kOutput };

struct ChannelSpec {
  std::string name;
  Port port;
  int depth;
};

struct Binding {
  std::string node;
  std::string channel;
};

struct InstanceSpec {
  std::string name;
  std::string proc;
  std::vector<Binding> bindings;
};

struct ModelSpec {
  std::vector<ProcSpec> procs;
  std::vector<ChannelSpec> channels;
  std::vector<InstanceSpec> instances;
};

// Simulates a model with the scheduled IR from which it was generated.
int RunModel(int argc, char** argv, const ModelSpec& model);

}  // namespace xls::stage_gfsim

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_MODEL_H_
