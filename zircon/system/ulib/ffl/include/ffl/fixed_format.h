// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <ffl/utility.h>

namespace ffl {

// Forward declaration.
template <typename Integer, size_t FractionalBits>
struct FixedFormat;

// Type representing an intermediate value of a given FixedFormat.
template <typename>
struct Value;

template <typename Integer, size_t FractionalBits>
struct Value<FixedFormat<Integer, FractionalBits>> {
  using Format = FixedFormat<Integer, FractionalBits>;
  using Intermediate = typename Format::Intermediate;

  explicit constexpr Value(Intermediate value) : value{value} {}
  const Intermediate value;
};

// Type representing the format of a fixed-point value in terms of the
// underlying integer type and fractional precision. Provides key constants and
// operations for fixed-point computation and format manipulation.
template <typename Integer_, size_t FractionalBits_>
struct FixedFormat {
  // The underlying integral type of the fixed-point values in this format.
  using Integer = Integer_;

  // The intermediate integral type used by computations in this format.
  using Intermediate = typename IntermediateType<Integer>::Type;

  // Numeric constants for fixed-point computations.
  static constexpr size_t Bits = sizeof(Integer) * 8;
  static constexpr size_t IntermediateBits = sizeof(Intermediate) * 8;
  static constexpr size_t FractionalBits = FractionalBits_;
  static constexpr size_t IntegralBits = Bits - FractionalBits;
  static constexpr size_t Power = 1 << FractionalBits;

  static constexpr Integer One = 1;  // Typed constant used in shifts below.
  static constexpr Integer FractionalMask = Power - 1;
  static constexpr Integer IntegralMask = ~FractionalMask;
  static constexpr Integer SignBit = std::is_signed_v<Integer> ? One << (Bits - 1) : 0;
  static constexpr Integer BinaryPoint = FractionalBits > 0 ? One << (FractionalBits - 1) : 0;
  static constexpr Integer OnesPlace = One << FractionalBits;

  static constexpr Integer Min = std::numeric_limits<Integer>::min();
  static constexpr Integer Max = std::numeric_limits<Integer>::max();
  static constexpr Integer IntegralMin = static_cast<Integer>(Min / Power);
  static constexpr Integer IntegralMax = static_cast<Integer>(Max / Power);

  // Assert that the template arguments are valid.
  static_assert(std::is_integral_v<Integer>,
                "The Integer template parameter must be an integral type!");
  static_assert((std::is_signed_v<Integer> && FractionalBits < Bits) ||
                    (!std::is_signed_v<Integer> && FractionalBits <= Bits),
                "The number of fractional bits must fit within the positive bits!");

  // Trivially converts from Integer to Intermediate type.
  static constexpr Intermediate ToIntermediate(Intermediate value) { return value; }

  // Saturates an intermediate value to the valid range of the base type.
  static constexpr Integer Saturate(Intermediate value) {
    const Intermediate clamped = std::clamp(value, Intermediate{Min}, Intermediate{Max});
    return static_cast<Integer>(clamped);
  }
  static constexpr Integer Saturate(Value<FixedFormat> value) { return Saturate(value.value); }

  // Rounds |value| to the given significant bit |Place| using the convergent,
  // or round-half-to-even, method to eliminate positive/negative and
  // towards/away from zero biases. This is the default rounding mode used in
  // IEEE 754 computing functions and operators.
  //
  // References:
  //   https://en.wikipedia.org/wiki/Rounding#Round_half_to_even
  //   https://en.wikipedia.org/wiki/Nearest_integer_function
  //
  // Optimization Analysis:
  //   https://godbolt.org/z/Cozc9r
  //
  // For example, rounding an 8bit value to bit 4 produces these values in the
  // constants defined below:
  //
  // uint8_t value = vvvphmmm
  //
  // PlaceBit   = 00010000 -> 000p0000
  // PlaceMask  = 11110000 -> vvvp0000
  // HalfBit    = 00001000 -> 0000h000
  // HalfMask   = 00000111 -> 00000mmm
  // PlaceShift = 2
  //
  // Rounding half to even is computed as follows:
  //
  //    PlaceBit = 00010000
  //    value    = vvvvvvvv
  // &  -------------------
  //               000p0000
  //    PlaceShift        2
  // >> -------------------
  //    odd_bit    00000p00
  //    HalfMask   00000111
  //    value      vvvvvvvv
  // +  -------------------
  //               rrrrxxxx
  //    PlaceMask  11110000
  // &  -------------------
  //    rounded    rrrr0000
  //
  template <size_t Place>
  static constexpr Intermediate Round(Intermediate value, Bit<Place>) {
    // Bit of the significant figure to round to and mask of the significant
    // bits after rounding.
    const Intermediate PlaceBit = 1 << Place;
    const Intermediate PlaceMask = ~(PlaceBit - 1);

    // Bit representing one half of the significant figure to round to
    // and mask of the bits below it, if any.
    const Intermediate HalfBit = 1 << (Place - 1);
    const Intermediate HalfMask = Place > 1 ? HalfBit - 1 : 0;

    // Shift representing where to add the odd bit when rounding to even.
    const size_t PlaceShift = Place > 1 ? 2 : 1;

    // Compute a mask and bit to conditionally convert |value| to positive.
    // When |value| is negative then |mask| = -1 and |one| = 1, otherwise
    // both are zero. This optimizes out when |value| is unsigned.
    const Intermediate mask = static_cast<Intermediate>(-(value < 0));
    const Intermediate one = mask & 1;

    // Compute the absolute value of |value| using two's complement. This
    // optimizes out when |value| is unsigned.
    const auto absolute = static_cast<Intermediate>((value ^ mask) + one);

    // Round half to even.
    const Intermediate odd_bit = (absolute & PlaceBit) >> PlaceShift;
    const auto rounded = static_cast<Intermediate>((absolute + HalfMask + odd_bit) & PlaceMask);

    // Restore original sign. This optimizes out when |value| is unsigned.
    return static_cast<Intermediate>((rounded ^ mask) + one);
  }

  // Rounding to the 0th bit is a no-op.
  static constexpr Intermediate Round(Intermediate value, Bit<0>) { return value; }

  // Rounds the intermediate |value| around the integer position.
  static constexpr Intermediate Round(Intermediate value) {
    return Round(value, ToPlace<FractionalBits>);
  }

  // Converts an intermediate value in SourceFormat to this format, rounding
  // as necessary.
  template <typename SourceFormat>
  static constexpr Value<FixedFormat> Convert(Value<SourceFormat> value) {
    using IntermediateFormat =
        std::conditional_t<SourceFormat::Bits >= Bits, SourceFormat, FixedFormat>;
    using ValueType = typename IntermediateFormat::Intermediate;

    if constexpr (SourceFormat::FractionalBits >= FractionalBits) {
      const size_t delta = SourceFormat::FractionalBits - FractionalBits;
      const ValueType power = ValueType{1} << delta;
      const ValueType converted_value =
          IntermediateFormat::Round(value.value, ToPlace<delta>) / power;
      return Value<FixedFormat>{static_cast<Intermediate>(converted_value)};
    } else {
      const size_t delta = FractionalBits - SourceFormat::FractionalBits;
      const ValueType power = ValueType{1} << delta;
      const ValueType converted_value = static_cast<ValueType>(value.value * power);
      return Value<FixedFormat>{static_cast<Intermediate>(converted_value)};
    }
  }

  // Converting to the same format is a no-op.
  static constexpr Value<FixedFormat> Convert(Value<FixedFormat> value) { return value; }
};

}  // namespace ffl
