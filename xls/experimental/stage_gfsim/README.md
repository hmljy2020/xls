# Stage-level XLS models for GFSIM

## Design intent

Generate readable C++ cycle models directly from XLS `scheduled_proc` IR.
Model stage-level ready/valid backpressure and state dependencies to estimate
workload time at a supplied clock period.

Each executing proc definition has a concrete `gfsim::SimModule` with named
members for the data between stages and for persistent proc state. Its `Work()`
calls the generated stage methods from downstream to upstream. A stage reads
the current members and records pending changes when it advances. `Xfer()`
publishes those changes for the next cycle. Stage methods use XLS bit operations
and fall back to the XLS node interpreter. The simulator reads the matching IR at
startup for state initialization and interpreter lookups.

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

All three simulators ran the 451-instruction RISC-V workload with the same
schedule, inputs, and depth-two external FIFOs. Verilator 5.052 compiled
SystemVerilog emitted from the same Block IR used by the XLS interpreter. Its
outputs and handshake cycles matched across both scenarios.

GFSIM and the XLS Block Interpreter used Bazel `-c opt`. Verilator ran in
single-threaded mode with its C++ model built using Clang 22.1.8 at `-O2`.
Runs were interleaved on one pinned HiSilicon Kunpeng-920 core (aarch64). The
table reports median wall-clock times from five runs per simulator and scenario
after one warmup.

Timing covers the simulation loop through the final output, including FIFO
handling. All timed runs disable trace formatting and output. Model generation,
compilation, Block IR lowering, IR and stimulus parsing, initialization, and
reset are outside the timed region.

| Scenario | Simulated cycles | GFSIM (ms) | Block interpreter (ms) | Verilator (ms) | Block / GFSIM | GFSIM / Verilator |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Steady | 1356 | 104.0 | 564.9 | 0.635 | 5.43× | 163.7× |
| Backpressure | 1483 | 109.9 | 617.5 | 0.687 | 5.62× | 160.0× |
