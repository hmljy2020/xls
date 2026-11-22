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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "SimModule.h"
#include "absl/container/inlined_vector.h"
#include "absl/types/span.h"
#include "xls/experimental/stage_gfsim/model.h"
#include "xls/ir/value.h"

namespace xls {
class Node;
class Proc;

namespace stage_gfsim {

class ChannelOwner : public gfsim::SimModule {
 public:
  using SimModule::SimModule;

  template <typename Data>
  gfsim::SimQueue<Data>* Attach(std::shared_ptr<gfsim::SimQueue<Data>> queue,
                                Port role) {
    auto* port = AddInnerQueue(std::move(queue));
    if (role == Port::kInput) {
      BindOutput(port);
    } else if (role == Port::kOutput) {
      BindInput(port);
    }
    return port;
  }
};

// The network drives all channels uniformly. Generated modules bind the
// concrete typed queues and move data without converting to XLS Value.
class SimChannel {
 public:
  virtual ~SimChannel() = default;
  virtual void Build(ChannelOwner& owner, const ChannelSpec& spec,
                     bool trace) = 0;
  virtual void AddInput(const Value& input) = 0;
  virtual void ResetInputs() = 0;
  virtual void Sample() = 0;
  virtual bool Source(gfsim::SimModule& module) = 0;
  virtual void Consume(gfsim::SimModule& module,
                       std::string_view kind) const = 0;
  virtual void Produce(gfsim::SimModule& module, const void* data) const = 0;

  bool readable = false;
  bool writable = false;
};

void TraceTransfer(gfsim::SimModule& module, std::string_view kind,
                   const gfsim::SimObj& queue, const Value& value);

struct ValueCodec {
  using Type = Value;
  static const Value& FromValue(const Value& value) { return value; }
  static const Value& ToValue(const Value& value) { return value; }
};

template <typename Codec>
class TypedChannel final : public SimChannel {
 public:
  using Data = typename Codec::Type;
  using Queue = gfsim::SimQueue<Data>;

  Queue* queue() const { return queue_; }

  void Build(ChannelOwner& owner, const ChannelSpec& spec,
             bool trace) override {
    auto queue = std::make_shared<Queue>(spec.name);
    queue->InitMaxSize(spec.depth);
    queue->Setlatency(1);
    queue_ = owner.Attach(std::move(queue), spec.port);
    trace_ = trace;
  }

  void AddInput(const Value& input) override {
    inputs_.push_back(MakePacket(Codec::FromValue(input)));
  }
  void ResetInputs() override { next_input_ = 0; }

  void Sample() override {
    readable = !queue_->Empty();
    writable = !queue_->Full();
  }

  bool Source(gfsim::SimModule& module) override {
    if (!writable || next_input_ == inputs_.size()) {
      return false;
    }
    const Packet& input = inputs_[next_input_++];
    if (trace_) {
      TraceTransfer(module, "source", *queue_, Codec::ToValue(DataOf(input)));
    }
    queue_->Write(input);
    return true;
  }

  void Consume(gfsim::SimModule& module, std::string_view kind) const override {
    if (trace_) {
      TraceTransfer(module, kind, *queue_, Codec::ToValue(queue_->Peek()));
    }
    queue_->Pop();
  }

  void Produce(gfsim::SimModule& module, const void* data) const override {
    const Data& value = *static_cast<const Data*>(data);
    queue_->Write(MakePacket(value));
    if (trace_) {
      TraceTransfer(module, "send", *queue_, Codec::ToValue(value));
    }
  }

 private:
  using Packet = typename Queue::Packet;

  // GFSIM queues store scalars directly and class payloads by shared pointer.
  static Packet MakePacket(const Data& data) {
    if constexpr (std::is_same_v<Data, Packet>) {
      return data;
    } else {
      return std::make_shared<Data>(data);
    }
  }
  static const Data& DataOf(const Packet& packet) {
    if constexpr (std::is_same_v<Data, Packet>) {
      return packet;
    } else {
      return *packet;
    }
  }

  Queue* queue_ = nullptr;
  bool trace_ = false;
  std::vector<Packet> inputs_;
  size_t next_input_ = 0;
};

template <typename Codec>
std::unique_ptr<SimChannel> MakeChannel() {
  return std::make_unique<TypedChannel<Codec>>();
}

// IR lookups and counters for one generated proc module.
class ProcRuntime {
 public:
  ProcRuntime(gfsim::SimModule& module, Proc& ir, std::size_t stage_count,
              bool interpret);
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

  std::optional<Value> Receive(const TypedChannel<ValueCodec>& channel,
                               std::string_view node, bool enabled = true);
  template <typename Codec>
  const typename Codec::Type* ReceiveData(const TypedChannel<Codec>& channel) {
    if (!ReceiveReady(channel)) {
      return nullptr;
    }
    return &channel.queue()->Peek();
  }
  template <typename Codec>
  bool Send(const TypedChannel<Codec>& channel,
            const typename Codec::Type& data, bool enabled = true) {
    return ProposeSend(channel, &data, enabled);
  }
  bool StateReady(bool available);
  void Finish();

 private:
  struct SendEffect {
    const SimChannel* channel;
    // The generated stage keeps the data alive through Finish().
    const void* data;
  };

  bool ReceiveReady(const SimChannel& channel);
  bool ProposeSend(const SimChannel& channel, const void* data, bool enabled);

  ProcRuntime& proc_;
  std::size_t index_;
  absl::InlinedVector<const SimChannel*, 1> receives_;
  std::optional<SendEffect> send_;
};

}  // namespace stage_gfsim
}  // namespace xls

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_PROC_RUNTIME_H_
