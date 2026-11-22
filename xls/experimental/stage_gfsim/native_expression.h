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

#ifndef XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_EXPRESSION_H_
#define XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_EXPRESSION_H_

#include <string>
#include <unordered_map>

namespace xls {
class Node;
class Type;
class Value;

namespace stage_gfsim {

using Variables = std::unordered_map<const Node*, std::string>;

// Empty results indicate that the original XLS Value backend is needed.
std::string NativeCodec(const Type* type);
std::string NativeExpression(const Node& node, const Variables& names);
std::string NativeLiteral(const Type* type, const Value& value);

}  // namespace stage_gfsim
}  // namespace xls

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_EXPRESSION_H_
