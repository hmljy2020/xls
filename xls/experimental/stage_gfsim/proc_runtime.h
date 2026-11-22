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

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// Channels are owned by the network. Admission is snapshotted at the
// beginning of Work, so each endpoint sees the same one-cycle FIFO boundary.
class SimChannel {
 public:
  virtual ~SimChannel() = default;
  virtual void Consume(gfsim::SimModule& module, std::string_view kind) = 0;
  virtual void Produce(gfsim::SimModule& module, const void* data) = 0;

  bool readable = false;
  bool writable = false;
};

void TraceTransfer(uint64_t cycle, gfsim::SimModule& module,
                   std::string_view kind, std::string_view queue,
                   const Value& value);

struct ValueCodec {
  using Type = Value;
  static const Value& FromValue(const Value& value) { return value; }
  static const Value& ToValue(const Value& value) { return value; }
};

template <typename Codec>
class TypedChannel final : public SimChannel {
 public:
  using Data = typename Codec::Type;

  TypedChannel(const ChannelSpec& spec, const RunOptions& options,
               const uint64_t& cycle)
      : name_(spec.name),
        cycle_(cycle),
        trace_(options.trace),
        stall_first_(spec.name == options.stall_output ? options.stall_first
                                                       : 0),
        stall_last_(spec.name == options.stall_output ? options.stall_last : 0),
        slots_(std::bit_ceil(static_cast<unsigned>(spec.depth))),
        mask_(slots_.size() - 1),
        capacity_(spec.depth) {}

  void LoadInputs(const Stimuli& stimuli) {
    for (const Value& input : stimuli.at(std::string(name_))) {
      inputs_.push_back(Codec::FromValue(input));
    }
  }

  void Reset() { head_ = tail_ = size_ = next_input_ = 0; }

  void Sample() {
    readable = (size_ != 0);
    writable = (size_ < capacity_);
  }

  const Data& Peek() const { return slots_[head_]; }

  template <bool kTrace>
  bool Source(gfsim::SimModule& module) {
    if (!writable || next_input_ == inputs_.size()) {
      return false;
    }
    Push<kTrace>(module, "source", inputs_[next_input_++]);
    return true;
  }

  template <bool kTrace>
  bool Sink(gfsim::SimModule& module, uint64_t cycle) {
    if (!readable || cycle - stall_first_ <= stall_last_ - stall_first_) {
      return false;
    }
    CommitRead<kTrace>(module, "sink");
    return true;
  }

  template <bool kTrace>
  void CommitRead(gfsim::SimModule& module, std::string_view kind) {
    if constexpr (kTrace) {
      TraceTransfer(cycle_, module, kind, name_, Codec::ToValue(Peek()));
    }
    head_ = Advance(head_);
    --size_;
  }

  template <bool kTrace>
  void CommitWrite(gfsim::SimModule& module, const Data& data) {
    Push<kTrace>(module, "send", data);
  }

  void Consume(gfsim::SimModule& module, std::string_view kind) override {
    if (trace_) {
      CommitRead<true>(module, kind);
    } else {
      CommitRead<false>(module, kind);
    }
  }

  void Produce(gfsim::SimModule& module, const void* data) override {
    const auto& value = *static_cast<const Data*>(data);
    if (trace_) {
      CommitWrite<true>(module, value);
    } else {
      CommitWrite<false>(module, value);
    }
  }

 private:
  size_t Advance(size_t index) const { return (index + 1) & mask_; }

  template <bool kTrace>
  void Push(gfsim::SimModule& module, std::string_view kind, const Data& data) {
    slots_[tail_] = data;
    tail_ = Advance(tail_);
    ++size_;
    if constexpr (kTrace) {
      TraceTransfer(cycle_, module, kind, name_, Codec::ToValue(data));
    }
  }

  std::string_view name_;
  const uint64_t& cycle_;
  bool trace_;
  uint64_t stall_first_;
  uint64_t stall_last_;
  std::vector<Data> slots_;
  size_t mask_;
  size_t capacity_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t size_ = 0;
  std::vector<Data> inputs_;
  size_t next_input_ = 0;
};

// Clock and reporting shared by compiled networks. Each generated Work calls
// its concrete channels and procs in the same order as elaboration.
class NetworkRuntime : public gfsim::SimModule {
 public:
  explicit NetworkRuntime(gfsim::SimSystem& system)
      : SimModule("stage_gfsim", &system), system_(system) {}

  void Reset() override;
  void ReportStat() override;
  virtual void RunCompiled(uint64_t cycles) = 0;

 protected:
  const uint64_t& Cycle() const { return cycle_; }
  void AdvanceCycle() { ++cycle_; }

  template <bool kTrace, typename Codec>
  bool Source(TypedChannel<Codec>& channel) {
    if (channel.template Source<kTrace>(*this)) {
      if (first_source_ == 0) {
        first_source_ = cycle_;
      }
      return true;
    }
    return false;
  }

  template <bool kTrace, typename Codec>
  bool Sink(TypedChannel<Codec>& channel) {
    if (channel.template Sink<kTrace>(*this, cycle_)) {
      last_sink_ = cycle_;
      ++delivered_;
      return true;
    }
    return false;
  }

  void MarkProgress() { system_.MarkProgress(); }

 private:
  gfsim::SimSystem& system_;
  uint64_t cycle_ = 0;
  uint64_t delivered_ = 0;
  uint64_t first_source_ = 0;
  uint64_t last_sink_ = 0;
};

// Large states publish by changing the bank index, without another copy on
// Xfer. Work can still read the old state after proposing a write.
template <typename Data>
class BufferedState {
 public:
  const Data& Current() const { return banks_[current_]; }
  void Reset(const Data& initial) {
    current_ = 0;
    pending_ = false;
    banks_[0] = initial;
  }
  void Propose(const Data& value) {
    banks_[current_ ^ 1] = value;
    pending_ = true;
  }
  void Xfer() {
    current_ ^= pending_;
    pending_ = false;
  }

 private:
  std::array<Data, 2> banks_;
  unsigned current_ = 0;
  bool pending_ = false;
};

// IR lookups and counters for one generated proc module.
class ProcRuntime {
 public:
  ProcRuntime(gfsim::SimModule& module, Proc& ir, std::size_t stage_count,
              bool interpret);
  void Reset();
  const Value& InitialState(std::string_view name) const;
  bool StageReady(std::size_t index, bool input_valid, bool output_ready) {
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

  bool InputReady(std::size_t index, bool ready) {
    if (!ready) {
      ++stats_[index].input;
    }
    return ready;
  }

  bool OutputReady(std::size_t index, bool ready) {
    if (!ready) {
      ++stats_[index].output;
    }
    return ready;
  }

  bool StateReady(std::size_t index, bool available) {
    if (!available) {
      ++stats_[index].state;
    }
    return available;
  }

  void BeginCycle() { progressed_ = false; }
  bool Progressed() const { return progressed_; }

  void StageFired(std::size_t index) {
    ++stats_[index].fired;
    progressed_ = true;
  }
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
  bool progressed_ = false;
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

  std::optional<Value> Receive(TypedChannel<ValueCodec>& channel,
                               std::string_view node, bool enabled = true);
  template <typename Codec>
  const typename Codec::Type* ReceiveData(TypedChannel<Codec>& channel) {
    if (!ReceiveReady(channel)) {
      return nullptr;
    }
    return &channel.Peek();
  }
  template <typename Codec>
  bool Send(TypedChannel<Codec>& channel, const typename Codec::Type& data,
            bool enabled = true) {
    return ProposeSend(channel, &data, enabled);
  }
  void Finish();

 private:
  struct SendEffect {
    SimChannel* channel;
    // The generated stage keeps the data alive through Finish().
    const void* data;
  };

  bool ReceiveReady(SimChannel& channel);
  bool ProposeSend(SimChannel& channel, const void* data, bool enabled);

  ProcRuntime& proc_;
  std::size_t index_;
  absl::InlinedVector<SimChannel*, 1> receives_;
  std::optional<SendEffect> send_;
};

}  // namespace stage_gfsim
}  // namespace xls

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_PROC_RUNTIME_H_
