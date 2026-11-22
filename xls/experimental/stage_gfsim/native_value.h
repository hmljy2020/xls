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

#ifndef XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_VALUE_H_
#define XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_VALUE_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "xls/ir/bits.h"
#include "xls/ir/value.h"

namespace xls::stage_gfsim {

// Widths are part of the IR codec. The C++ storage uses ordinary unsigned
// integers, so generated expressions explicitly truncate where the IR wraps.
template <int Width>
using NativeUInt = std::conditional_t<
    (Width <= 8), uint8_t,
    std::conditional_t<(Width <= 16), uint16_t,
                       std::conditional_t<(Width <= 32), uint32_t, uint64_t>>>;

template <int Width>
constexpr NativeUInt<Width> ClipBits(uint64_t value) {
  if constexpr (Width == 64) {
    return value;
  } else {
    return value & ((uint64_t{1} << Width) - 1);
  }
}

template <int Width>
constexpr int64_t SignedBits(uint64_t value) {
  if constexpr (Width == 0) {
    return 0;
  } else {
    constexpr uint64_t sign = uint64_t{1} << (Width - 1);
    return std::bit_cast<int64_t>((value ^ sign) - sign);
  }
}

template <int Width>
constexpr NativeUInt<Width> ShiftLeft(uint64_t value, uint64_t amount) {
  return amount < Width ? ClipBits<Width>(value << amount) : 0;
}

template <int Width>
constexpr NativeUInt<Width> ShiftRight(uint64_t value, uint64_t amount) {
  return amount < Width ? ClipBits<Width>(value >> amount) : 0;
}

template <int Width>
constexpr NativeUInt<Width> ShiftRightSigned(uint64_t value, uint64_t amount) {
  const int64_t signed_value = SignedBits<Width>(value);
  return ClipBits<Width>(signed_value >> std::min<uint64_t>(amount, 63));
}

template <typename T, size_t Size>
const T& ArrayElement(const std::array<T, Size>& array, uint64_t index) {
  return array[std::min<uint64_t>(index, Size - 1)];
}

// Array updates outside the bounds leave the array unchanged.
template <typename T, size_t Size, typename Element, typename... Indices>
void SetArrayElement(std::array<T, Size>& array, const Element& value,
                     uint64_t index, Indices... rest) {
  if (index < Size) {
    if constexpr (sizeof...(rest) == 0) {
      array[index] = value;
    } else {
      SetArrayElement(array[index], value, rest...);
    }
  }
}

template <typename Array, typename Element, typename... Indices>
Array WithArrayElement(Array array, const Element& value, Indices... indices) {
  SetArrayElement(array, value, indices...);
  return array;
}

template <int Width>
struct NativeBits {
  using Type = NativeUInt<Width>;
  static Type FromValue(const Value& value) {
    return ClipBits<Width>(value.bits().ToUint64().value());
  }
  static Value ToValue(Type value) { return Value(UBits(value, Width)); }
};

struct NativeToken {
  using Type = std::tuple<>;
  static Type FromValue(const Value&) { return {}; }
  static Value ToValue(Type) { return Value::Token(); }
};

template <typename Element, size_t Size>
struct NativeArray {
  using Type = std::array<typename Element::Type, Size>;
  static Type FromValue(const Value& value) {
    Type data;
    for (size_t i = 0; i < Size; ++i) {
      data[i] = Element::FromValue(value.element(i));
    }
    return data;
  }
  static Value ToValue(const Type& data) {
    std::vector<Value> elements;
    elements.reserve(Size);
    for (const auto& element : data) {
      elements.push_back(Element::ToValue(element));
    }
    return Value::ArrayOwned(std::move(elements));
  }
};

template <typename... Elements>
struct NativeTuple {
  using Type = std::tuple<typename Elements::Type...>;
  static Type FromValue(const Value& value) {
    return [&]<size_t... I>(std::index_sequence<I...>) {
      return Type{Elements::FromValue(value.element(I))...};
    }(std::index_sequence_for<Elements...>{});
  }
  static Value ToValue(const Type& data) {
    return [&]<size_t... I>(std::index_sequence<I...>) {
      std::vector<Value> values;
      values.reserve(sizeof...(Elements));
      (values.push_back(Elements::ToValue(std::get<I>(data))), ...);
      return Value::TupleOwned(std::move(values));
    }(std::index_sequence_for<Elements...>{});
  }
};

}  // namespace xls::stage_gfsim

#endif  // XLS_EXPERIMENTAL_STAGE_GFSIM_NATIVE_VALUE_H_
