#!/usr/bin/env bash
# Copyright 2026 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

experiment=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$experiment/../../.."
target=xls/experimental/stage_gfsim
bazel=${BAZEL:-$(command -v bazelisk || command -v bazel)}
bin=$PWD/bazel-bin
name=riscv
workload=short
convert_options=(--emit_assert=false)
opt_options=(--skip_passes=next_value_opt)
if [[ ${1:-} == --default-pipeline ]]; then
  name=riscv_default
  convert_options=()
  opt_options=()
elif [[ ${1:-} == --long ]]; then
  workload=long
fi

"$bazel" build --lockfile_mode=off \
  //xls/dslx/ir_convert:ir_converter_main \
  //xls/tools:opt_main "//$target:schedule_proc"

out=$experiment/out/$name
mkdir -p "$out"
"$bin/xls/dslx/ir_convert/ir_converter_main" --top=Riscv \
  --lower_to_proc_scoped_channels=true --dslx_stdlib_path="$PWD/xls/dslx/stdlib" \
  --dslx_path="$PWD" "${convert_options[@]}" \
  "$experiment/examples/riscv.x" > "$out/raw.ir"
"$bin/xls/tools/opt_main" "${opt_options[@]}" "$out/raw.ir" > "$out/opt.ir"
"$bin/$target/schedule_proc" --stages=4 --ii=4 \
  --schedule_out="$out/schedule.textproto" "$out/opt.ir" "$out/scheduled.ir"
"$experiment/build_model.sh" "$out/scheduled.ir" "$name"

python3 - "$bin/$target/out/$name/sim" "$out" "$workload" <<'PY'
import ast
import csv
from pathlib import Path
import re
import subprocess
import sys

sim, directory, workload = sys.argv[1:]
out = Path(directory)
schedule = out / "scheduled.ir"

# Encodings and state changes from risc_v_example_test in riscv_simple.x.
short_program = [
    (0x0020C1B3, 0x04, {3: 3}, {}),          # xor x3, x1, x2
    (0x00318333, 0x08, {6: 6}, {}),          # add x6, x3, x3
    (0x00602223, 0x0C, {}, {4: 6}),          # sw x6, 4(x0)
    (0x00130393, 0x10, {7: 7}, {}),          # addi x7, x6, 1
    (0x00402403, 0x14, {8: 6}, {}),          # lw x8, 4(x0)
    (0x00740333, 0x18, {6: 13}, {}),         # add x6, x8, x7
    (0x080300E7, 0x8C, {1: 0x1C}, {}),       # jalr x1, 128(x6)
    (0x800014B7, 0x90, {9: 0x80001000}, {}), # lui x9, 0x80001
    (0x0100056F, 0xA0, {10: 0x94}, {}),      # jal x10, 16
    (0x00630863, 0xB0, {}, {}),              # beq x6, x6, 16
    (0x00631863, 0xB4, {}, {}),              # bne x6, x6, 16
    (0x0020D863, 0xC4, {}, {}),              # bge x1, x2, 16
    (0xFE1148E3, 0xB4, {}, {}),              # blt x2, x1, -16
]


def loop_program():
    # Branch at PC 0x20 returns to 0x08 until x4 reaches zero.
    iterations = 64
    word = 85
    checksum = 0
    yield 0x04000213, 0x04, {4: iterations}, {}    # addi x4, x0, 64
    yield 0x05500293, 0x08, {5: word}, {}          # addi x5, x0, 85
    for remaining in range(iterations - 1, -1, -1):
        word += 1
        yield 0x001282B3, 0x0C, {5: word}, {}      # add x5, x5, x1
        word ^= 2
        yield 0x0022C2B3, 0x10, {5: word}, {}      # xor x5, x5, x2
        yield 0x00502223, 0x14, {}, dict(enumerate(word.to_bytes(4, "little"), 4))
        yield 0x00402303, 0x18, {6: word}, {}      # lw x6, 4(x0)
        checksum += word
        yield 0x006383B3, 0x1C, {7: checksum}, {}  # add x7, x7, x6
        yield 0x40120233, 0x20, {4: remaining}, {} # sub x4, x4, x1
        yield 0xFE0214E3, 0x08 if remaining else 0x24, {}, {}  # bne x4, x0, -24
    yield 0x0072C433, 0x28, {8: word ^ checksum}, {}           # xor x8, x5, x7


if workload == "long":
    out = out / "long"
    out.mkdir(exist_ok=True)
    program = list(loop_program())
    max_cycles = 2048
    stall_first, stall_last = 80, 211
else:
    program = short_program
    max_cycles = 128
    stall_first, stall_last = 6, 22

inputs = out / "inputs.txt"
inputs.write_text("".join(f"_instruction bits[32]:0x{word:08x}\n" for word, *_ in program))

registers = [0, 1, 2] + [0] * 29
memory = [0] * 16
expected = []
for _, pc, register_writes, memory_writes in program:
    for register, value in register_writes.items():
        registers[register] = value
    for address, value in memory_writes.items():
        memory[address] = value
    expected.append((pc, registers.copy(), memory.copy()))

stall_flags = [
    "--stall_output=_result", f"--stall_first={stall_first}", f"--stall_last={stall_last}"
]
for tag, flags in (("steady", []), ("stalled", stall_flags)):
    trace_path = out / f"{tag}.tsv"
    with trace_path.open("w") as trace, (out / f"{tag}.stats").open("w") as stats:
        subprocess.run(
            [sim, schedule, f"--inputs={inputs}", f"--cycles={max_cycles}",
             "--clock_period_ps=1000", *flags],
            check=True, stdout=trace, stderr=stats,
        )
    with trace_path.open() as trace:
        rows = list(csv.DictReader(trace, delimiter="\t"))
    outputs = [row for row in rows if row["event"] == "sink"]
    assert len(outputs) == len(expected), (tag, len(outputs))
    for index, (row, state) in enumerate(zip(outputs, expected), 1):
        actual = ast.literal_eval(re.sub(r"bits\[\d+\]:", "", row["value"]))
        assert actual == state, (tag, index, actual, state)
    cycles = [int(row["cycle"]) for row in outputs]
    if flags:
        assert all(not stall_first <= cycle <= stall_last for cycle in cycles), cycles
    first_source = next(int(row["cycle"]) for row in rows if row["event"] == "source")
    print(f"{tag}: {len(outputs)} instruction states, {cycles[-1] - first_source + 1} workload cycles")
PY
