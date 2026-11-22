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
"$bazel" build --lockfile_mode=off \
  //xls/dslx/ir_convert:ir_converter_main \
  //xls/tools:opt_main "//$target:schedule_proc"
bin=$PWD/bazel-bin
mkdir -p "$experiment/out"
inputs=$experiment/out/inputs.txt
for value in {1..18}; do
  printf '_input bits[32]:%d\n' "$value"
done > "$inputs"

build_example() {
  local name=$1 top=$2
  shift 2
  local out=$experiment/out/$name
  mkdir -p "$out"
  "$bin/xls/dslx/ir_convert/ir_converter_main" \
    --top="$top" --lower_to_proc_scoped_channels=true --dslx_stdlib_path="$PWD/xls/dslx/stdlib" \
    "$experiment/examples/$name.x" > "$out/raw.ir"
  "$bin/xls/tools/opt_main" "$out/raw.ir" > "$out/opt.ir"
  "$bin/$target/schedule_proc" --schedule_out="$out/schedule.textproto" \
    "$@" "$out/opt.ir" "$out/scheduled.ir"
  "$experiment/build_model.sh" "$out/scheduled.ir" "$name"
}

run_example() {
  local name=$1 tag=$2
  shift 2
  local out=$experiment/out/$name
  "$bin/$target/out/$name/sim" "$out/scheduled.ir" --inputs="$inputs" \
    --cycles=128 --clock_period_ps=1000 "$@" \
    > "$out/$tag.tsv" 2> "$out/$tag.stats"
}

build_example pipeline Pipeline --stages=3
run_example pipeline steady
run_example pipeline stalled --stall_output=_output --stall_first=6 --stall_last=16
build_example accumulator Accumulator --stages=6 --ii=6 --clock_period_ps=2
run_example accumulator steady
build_example buffered_pair BufferedPair --stages=2
run_example buffered_pair stalled --stall_output=_output --stall_first=8 --stall_last=24

python3 - "$experiment/out" <<'PY'
import csv
from pathlib import Path
import re
import sys

out = Path(sys.argv[1])
inputs = range(1, 19)

def events(example, run, event, channel=None):
    with (out / example / (run + ".tsv")).open() as trace:
        return [
            (int(row["cycle"]), int(row["value"].split(":")[1], 0))
            for row in csv.DictReader(trace, delimiter="\t")
            if row["event"] == event
            and (channel is None or channel in row["channel"])
        ]

expected = [(x * x + x) ^ 7 for x in inputs]
steady = events("pipeline", "steady", "send", "_output")
assert [v for _, v in steady] == expected, steady
cycles = [c for c, _ in steady]
assert cycles == list(range(cycles[0], cycles[0] + len(expected))), cycles
stalled = events("pipeline", "stalled", "sink")
assert [v for _, v in stalled] == expected, stalled
assert all(not 6 <= c <= 16 for c, _ in stalled), stalled
stats = (out / "pipeline/stalled.stats").read_text()
assert re.search(r"stage_0\.output_stall_cycles\.*:\s*[1-9]", stats), stats

expected = []
state = 0
for x in inputs:
    state = (((state + x * x) ^ 7) * 3) & 0xFFFFFFFF
    expected.append(state)
accumulated = events("accumulator", "steady", "sink")
assert [v for _, v in accumulated] == expected, accumulated
stats = (out / "accumulator/steady.stats").read_text()
assert re.search(r"\.state_stall_cycles\.*:\s*[1-9]", stats), stats

expected = [(x + 3) ** 2 for x in inputs]
buffered = events("buffered_pair", "stalled", "sink")
assert [v for _, v in buffered] == expected, buffered
writes = events("buffered_pair", "stalled", "send", "mid")
reads = events("buffered_pair", "stalled", "receive", "mid")
assert len(writes) == len(reads) == len(expected), (writes, reads)
assert all(r > w for (w, _), (r, _) in zip(writes, reads)), (writes, reads)
assert max(sum(w <= c for w, _ in writes) - sum(r <= c for r, _ in reads)
           for c, _ in writes) == 2, (writes, reads)
print("Passed 3 DSLX → scheduled_proc → GFSIM examples.")
PY
