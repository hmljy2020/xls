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

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_applicable_licenses = ["//:license"])

exports_files(["proc_runtime.h", "runtime.cc"])

cc_library(
    name = "model",
    hdrs = ["model.h"],
    visibility = ["//xls/experimental/stage_gfsim:__subpackages__"],
    deps = ["@abseil-cpp//absl/status"],
)

cc_binary(
    name = "schedule_proc",
    srcs = ["schedule_proc.cc"],
    deps = [
        "//xls/codegen_v_1_5:scheduling_pass",
        "//xls/common:exit_status",
        "//xls/common:init_xls",
        "//xls/common/file:filesystem",
        "//xls/common/status:status_macros",
        "//xls/estimators/delay_model:delay_estimators",
        "//xls/estimators/delay_model/models:model_unit",
        "//xls/ir",
        "//xls/ir:ir_parser",
        "//xls/scheduling:scheduling_options",
        "//xls/tools:schedule",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/status",
    ],
)

cc_binary(
    name = "generate",
    srcs = ["generate.cc"],
    deps = [
        ":model",
        "//xls/common:exit_status",
        "//xls/common:init_xls",
        "//xls/common/file:filesystem",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/ir:channel",
        "//xls/ir:ir_parser",
        "//xls/ir:name_uniquer",
        "//xls/ir:op",
        "//xls/ir:proc_elaboration",
        "//xls/ir:state_element",
        "//xls/ir:value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)
