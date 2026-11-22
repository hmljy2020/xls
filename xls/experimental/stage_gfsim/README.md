# Stage-level XLS models for GFSIM

This experiment generates a C++ GFSIM model from XLS `scheduled_proc` IR.
The generated source defines one C++ function per stage and describes resolved
channel connections. Stage functions call XLS bit operations directly for
arithmetic and common data operations, and use the XLS node interpreter for
other operations. The simulator reads the matching IR at startup. A shared
runtime advances every proc's stages and commits its state each cycle.

## Run the examples

From the XLS root, with Bazelisk or Bazel 8.7.0 and a local GFSIM checkout:

```bash
GFSIM_DIR=/home/lc/GFSIM xls/experimental/stage_gfsim/run_examples.sh
```

The scripts prefer `bazelisk` when it is installed. Set `BAZEL` to choose
another executable. The script converts, optimizes, schedules, generates and
builds each of the three designs. Generated C++, IR, schedules, handshake traces
and counters are under `out/<example>/`.

In `model.cc`, the `Eval_…_StageN` functions show the stage inputs, operations,
channel accesses and values forwarded to the next stage. The model binds
channel operations to named channels for each proc instance.

## Build an existing scheduled design

```bash
xls/experimental/stage_gfsim/build_model.sh /path/to/design.scheduled.ir my_model
bazel-bin/xls/experimental/stage_gfsim/out/my_model/sim \
  /path/to/design.scheduled.ir --inputs=inputs.txt --cycles=128 \
  --clock_period_ps=1000
```

List one channel name followed by an XLS typed value per line in
`inputs.txt`. Values for each input are offered in order as its FIFO admits
them. For example:

```text
_input bits[32]:10
_input bits[32]:20
```

The input can also be built starting from optimized proc IR:

```bash
bazelisk build //xls/experimental/stage_gfsim:schedule_proc
bazel-bin/xls/experimental/stage_gfsim/schedule_proc \
  --stages=3 --ii=1 --schedule_out=/tmp/design.schedule.textproto \
  /tmp/design.opt.ir /tmp/design.scheduled.ir
```

The schedule helper defaults to XLS's synthetic `unit` delay model. A
simulation's `--clock_period_ps` represents the period assumed for time
estimates.

## Cycle model

- Stage 0 can start an activation when its inputs, state and output slot
  allow it. Each subsequent stage reads the previous cycle's valid slot.
  A stage claims space when the downstream slot is empty or its old value
  advances this cycle.
- A stage commits its enabled channel operations and state update on advance.
  An update in a later stage makes the state unavailable from the advancing
  read until the new value commits.
- Streaming links are GFSIM queues with one producer and one consumer. Writes
  become visible on the following cycle. A full queue at the beginning of a
  cycle blocks writes for that cycle. External ports and channels without an
  explicit depth have depth two.
- The supported subset uses blocking receives, one send per stage, one
  operation in each direction per channel in the network, and one unconditional
  read and at most one write per state element. Internal queues start empty and
  use positive depth with bypass disabled. FIFO port-register options and
  codegen input/output flops are outside this stage model.

The simulator writes TSV handshake events to stdout and counters to stderr.
`source` and `sink` events show the stimulus and consumer at the external
FIFOs. `receive` and `send` show each proc's own transfers. Cycle numbers start
at one. To apply backpressure, use `--stall_output=_output --stall_first=6
--stall_last=16`. Workload cycles count from the first source event through
the last sink event, inclusive. Estimated times are those cycle counts
multiplied by the supplied clock period.
