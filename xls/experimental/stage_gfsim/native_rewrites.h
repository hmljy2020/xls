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

#ifndef XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_REWRITES_H_
#define XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_REWRITES_H_

#include "absl/status/status.h"

namespace xls {
class Proc;

namespace stage_gfsim {

// Simplifies the host computation without moving the remaining stage effects.
absl::Status SimplifyNativeProc(Proc& proc);

}  // namespace stage_gfsim
}  // namespace xls

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_REWRITES_H_
