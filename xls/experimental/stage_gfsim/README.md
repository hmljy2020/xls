# Stage-level XLS models for GFSIM

## Design intent

Generate readable C++ cycle models directly from XLS `scheduled_proc` IR.
Model stage-level ready/valid backpressure and state dependencies to estimate
workload time at a supplied clock period.

Each executing proc definition has a concrete `gfsim::SimModule` with named
members for the data between stages and for persistent proc state. Its `Work()`
calls the generated stage methods from downstream to upstream. A stage reads
the current members and records pending changes when it advances. `Xfer()`
publishes those changes for the next cycle. Supported IR operations over bits
up to 64 wide, arrays, and tuples compile to native C++ expressions and
fixed-size storage. FIFO payloads use those C++ types, and conversion to XLS
`Value` is limited to input loading, initial state, and trace output.
If a model contains a type or operation the native emitter cannot handle,
the generator uses XLS bit operations and the node interpreter for that
model. Both paths read the matching scheduled IR at startup.

The generated network binds concrete procs and fixed-capacity FIFOs. Each
FIFO takes its admission decisions from the start of the cycle and holds
payloads in a preallocated ring. The default cycle driver calls the generated
`Work()` and `Xfer()` phases directly on the GFSIM module graph. Pass
`--gfsim_step` to run the same network through `gfsim::SimSystem::Step()`.
The native emitter may move pure expressions to their first use while
keeping channel transfers and state effects at their scheduled stages.

## Current status

The DSLX → scheduled proc → GFSIM flow runs three examples covering a
pipeline, a stateful accumulator, and two procs connected by a FIFO. The cycle
model supports blocking receives, at most one send per stage, a single producer
and consumer per channel, and empty, positive-depth FIFOs with bypass disabled.
Each state element can have one unconditional read and at most one write.

Run from the XLS root with Bazelisk or Bazel 8.7.0 and a local GFSIM checkout.

```bash
GFSIM_DIR=/home/lc/GFSIM xls/experimental/stage_gfsim/run_examples.sh
```

Each `out/<example>/` contains `scheduled.ir`, `model.cc`, handshake traces
(`.tsv`), and cycle counters (`.stats`). Use `build_model.sh <scheduled.ir>
<name>` to build a simulator for an existing schedule.

## RISC-V experiment

The RISC-V proc wraps the original `run_instruction()` and holds the
PC, registers, and data memory. The driver feeds the original test's 13
instructions and checks the full state returned after each instruction, with
and without output backpressure. [Proc source](examples/riscv.x) ·
[Instruction implementation](../../examples/riscv_simple.x)

```bash
GFSIM_DIR=/home/lc/GFSIM xls/experimental/stage_gfsim/run_riscv_example.sh
```

Pass `--long` for a 451-instruction workload. It checks all state snapshots
through 64 loop iterations with dependent arithmetic, store/load pairs, and
a conditional branch. The backpressure run holds the sink from cycle 80
through 211. Traces are under `out/riscv/long/`.

The valid-instruction run uses `--emit_assert=false` and
`--skip_passes=next_value_opt`. The default XLS flow emits IR assertions from
`fail!()` and splits some state updates into multiple predicated
`next_value` nodes. The generator supports at most one `next_value` per state
element and rejects assertions, including the scheduler's mutual-exclusion
checks for those split writes. Pass `--default-pipeline` to reproduce the
assertion rejection with the default XLS flags.

## Cycle comparison

XLS Codegen 1.5 lowers the same `scheduled.ir` to Block IR for the XLS
interpreter. For each scenario, both simulators use the same input sequence,
sink backpressure schedule, and depth-two external FIFOs. Workload cycles count
from the first accepted source value through the last sink transfer, inclusive,
after reset.

| Scenario | GFSIM cycles | Block IR cycles |
| --- | ---: | ---: |
| Pipeline, steady | 22 | 22 |
| Pipeline, backpressure | 33 | 33 |
| Accumulator, steady | 93 | 93 |
| Buffered pair, backpressure | 40 | 40 |
| RISC-V, steady | 42 | 42 |
| RISC-V, backpressure | 55 | 55 |
| RISC-V long, steady | 1356 | 1356 |
| RISC-V long, backpressure | 1483 | 1483 |

All output values and transfer cycles match. The short and long RISC-V
workloads also match on every input and output handshake. In the buffered pair,
ten proc input handshakes occur one cycle later in GFSIM. GFSIM checks FIFO
capacity at the start of the cycle. The Block IR internal FIFO can accept a
push while full if a pop occurs that cycle.

## Simulator runtime comparison

The simulators ran the 451-instruction RISC-V workload with the same schedule,
inputs, and depth-two external FIFOs. Verilator 5.052 compiled SystemVerilog
emitted from the same Block IR used by the XLS interpreter. All four variants
matched every transfer value and cycle across both scenarios.

GFSIM and the XLS Block Interpreter used Bazel `-c opt`. Verilator ran in
single-threaded mode with its C++ model built using Clang 22.1.8 at `-O2`.
Runs were interleaved on one pinned HiSilicon Kunpeng-920 core (aarch64). The
table reports median wall-clock times from seven runs per simulator and scenario
after one warmup.

Timing covers the simulation loop through the final output, including FIFO
handling. All timed runs disable trace formatting and output. Model generation,
compilation, Block IR lowering, IR and stimulus parsing, initialization, and
reset are outside the timed region.

| Scenario | Simulated cycles | Native GFSIM baseline (ms) | Verilator (ms) | GFSIM Value baseline (ms) | Block interpreter (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Steady | 1356 | 0.719 | 0.635 | 104.4 | 563.1 |
| Backpressure | 1483 | 0.742 | 0.689 | 110.8 | 617.2 |

The compiled cycle driver was measured on the same workload, compiler mode
and pinned core. These are medians from 11 interleaved runs after warmup. The
two drivers also match every transfer value, cycle and stage counter with
tracing enabled.

| Scenario | Compiled GFSIM (µs) | GFSIM Step (µs) | Verilator (µs) | Verilator / compiled GFSIM |
| --- | ---: | ---: | ---: | ---: |
| Steady | 60.8 | 86.2 | 634.5 | 10.43× |
| Backpressure | 63.5 | 91.2 | 685.9 | 10.81× |

To time the generated GFSIM model, generate the long example first and then
build its simulator in optimized mode. The program prints `simulation_ns`
with the workload counters. For the backpressure case, use `--cycles=1483
--stall_output=_result --stall_first=80 --stall_last=211`.

```bash
target=xls/experimental/stage_gfsim
"$target/run_riscv_example.sh" --long
bazel build -c opt --lockfile_mode=off "//$target/out/riscv:sim"
"bazel-bin/$target/out/riscv/sim" "$target/out/riscv/scheduled.ir" \
  --inputs="$target/out/riscv/long/inputs.txt" --cycles=1356 \
  --trace=false --simulation_time
```
