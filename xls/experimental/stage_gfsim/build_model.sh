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

if [[ $# != 2 || ! $2 =~ ^[[:alnum:]_]+$ ]]; then
  echo "Usage: $0 scheduled.ir model_name" >&2
  exit 2
fi

ir=$(realpath "$1")
name=$2
experiment=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$experiment/../../.."
target=xls/experimental/stage_gfsim
gfsim=${GFSIM_DIR:-/home/lc/GFSIM}
bazel=${BAZEL:-$(command -v bazelisk || command -v bazel)}

"$bazel" build --lockfile_mode=off "//$target:generate"
mkdir -p "$experiment/out/$name"
cp -R "$gfsim/include" "$gfsim/src" "$experiment/out/"
cp "$experiment/runtime.BUILD" "$experiment/out/BUILD"
bazel-bin/$target/generate "$ir" "$experiment/out/$name/model.cc"
cat > "$experiment/out/$name/BUILD" <<'BUILD'
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_applicable_licenses = ["//:license"])

cc_binary(
    name = "sim",
    srcs = ["model.cc"],
    deps = [
        "//xls/common/status:status_macros",
        "//xls/experimental/stage_gfsim:model",
        "//xls/experimental/stage_gfsim:native_value",
        "//xls/experimental/stage_gfsim/out:runtime",
        "//xls/ir",
        "//xls/ir:bits_ops",
        "@abseil-cpp//absl/status",
    ],
)
BUILD
"$bazel" build --lockfile_mode=off "//$target/out/$name:sim"
printf 'Executable: %s/bazel-bin/%s/out/%s/sim\n' "$PWD" "$target" "$name"
