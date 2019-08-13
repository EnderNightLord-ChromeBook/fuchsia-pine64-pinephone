// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

//
// Fuchsia Fixed-point Library (FFL):
//
// An efficient header-only multi-precision fixed point math library with well-
// defined rounding.
//

#include <type_traits>

#include <ffl/expression.h>
#include <ffl/fixed_format.h>
#include <ffl/utility.h>

namespace ffl {

// Represents a fixed-point value using the given integer base type |Integer|
// and the given number of fractional bits |FractionalBits|. This type supports
// standard arithmetic operations and comparisons between the same type, fixed-
// point types with different precision/resolution, and integer values.
//
// Arithmetic operations are not immediately computed. Instead, arithmetic
// expressions involving fixed-point types are assembled into intermediate
// expression trees (via the Expression template type) that capture operands and
// order of operations. The value of the expression tree is evaluated when it is
// assigned to a fixed-point variable. Using this approach the precision and
// resolution of intermediate values are selected at compile time, based on the
// final precision and resolution of the destination variable.
//
// See README.md for a more detailed discussion of fixed-point arithmetic,
// rounding, precision, and resolution in this library.
//
template <typename Integer, size_t FractionalBits>
class Fixed {
 public:
  // Alias of the FixedFormat type describing traits and low-level operations
  // on the fixed-point representation of this type.
  using Format = FixedFormat<Integer, FractionalBits>;

  // Fixed is default constructible without a default value, which is the same
  // as for plain integer types. This is permitted in constexpr contexts as
  // long as the underling integer member |value_| is initialized before use.
  constexpr Fixed() = default;

  // Fixed is copy constructible and assignable.
  constexpr Fixed(const Fixed&) = default;
  constexpr Fixed& operator=(const Fixed&) = default;

  // Explicit conversion from an integer value. The value is saturated to fit
  // within the integer precision defined by Format::IntegerBits.
  explicit constexpr Fixed(Integer value) : Fixed{ToExpression<Integer>{value}} {}

  // Implicit conversion from an intermediate expression. The value is rounded
  // and saturated to fit within the precision and resolution of this type, if
  // necessary.
  template <Operation Op, typename... Args>
  constexpr Fixed(Expression<Op, Args...> expression)
      : value_{Format::Saturate(Format::Convert(expression.Evaluate(Format{})))} {}

  // Assignment from an intermediate expression. The value is rounded and
  // saturated to fit within the precision and resolution of this type, if
  // necessary.
  template <Operation Op, typename... Args>
  constexpr Fixed& operator=(Expression<Op, Args...> expression) {
    return *this = Fixed{expression};
  }

  // Implicit conversion from an intermediate value of the same format.
  constexpr Fixed(Value<Format> value) : value_{Format::Saturate(value)} {}

  // Assignment from an intermediate value of the same format.
  constexpr Fixed& operator=(Value<Format> value) { return *this = Fixed{value}; }

  // Returns the raw fixed-point value as the underling integer type.
  constexpr Integer raw_value() const { return value_; }

  // Returns the fixed-point value as an intermediate value type.
  constexpr Value<Format> value() const { return Value<Format>{value_}; }

  // Returns the closest integer value greater-than or equal-to this fixed-
  // point value.
  constexpr Integer Ceiling() const {
    using Intermediate = typename Format::Intermediate;
    const Intermediate value = value_;
    const Intermediate power = Format::Power;
    const Intermediate saturated_value = Format::Saturate(value + Format::FractionalMask);
    return static_cast<Integer>(saturated_value / power);
  }

  // Returns the closest integer value less-than or equal-to this fixed-point
  // value.
  constexpr Integer Floor() const {
    using Intermediate = typename Format::Intermediate;
    const Intermediate power = Format::Power;
    const Intermediate value = value_ & Format::IntegralMask;
    return static_cast<Integer>(value / power);
  }

  // Returns the rounded value of this fixed-point value as an integer.
  constexpr Integer Round() const {
    using Intermediate = typename Format::Intermediate;
    const Intermediate power = Format::Power;
    const Intermediate rounded_value = Format::Round(value_);
    return Format::Saturate(static_cast<Intermediate>(rounded_value / power));
  }

  // Returns the fractional component of this fixed-point value.
  constexpr Fixed Fraction() const { return *this - Fixed{Floor()}; }

  // Relational operators for same-typed values.
  constexpr bool operator<(Fixed other) const { return value_ < other.value_; }
  constexpr bool operator>(Fixed other) const { return value_ > other.value_; }
  constexpr bool operator<=(Fixed other) const { return value_ <= other.value_; }
  constexpr bool operator>=(Fixed other) const { return value_ >= other.value_; }
  constexpr bool operator==(Fixed other) const { return value_ == other.value_; }
  constexpr bool operator!=(Fixed other) const { return value_ != other.value_; }

  // Compound assignment operators.
  template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
  constexpr Fixed& operator+=(T expression) {
    *this = *this + expression;
    return *this;
  }
  template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
  constexpr Fixed& operator-=(T expression) {
    *this = *this - expression;
    return *this;
  }
  template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
  constexpr Fixed& operator*=(T expression) {
    *this = *this * expression;
    return *this;
  }
  template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
  constexpr Fixed& operator/=(T expression) {
    *this = *this / expression;
    return *this;
  }

 private:
  Integer value_;
};

// Utility to round an expression to the given Integer.
template <typename Integer, typename T, typename Enabled = EnableIfUnaryExpression<T>>
inline constexpr auto Round(T expression) {
  const Fixed<Integer, 0> value{ToExpression<T>{expression}};
  return value.Round();
}

// Utility to create an Expression node from an integer value.
template <typename Integer, typename Enabled = std::enable_if_t<std::is_integral_v<Integer>>>
inline constexpr auto FromInteger(Integer value) {
  return ToExpression<Integer>{value};
}

// Utility to create an Expression node from an integer ratio. May be used to
// initialize a Fixed variable from a ratio.
template <typename Integer, typename Enabled = std::enable_if_t<std::is_integral_v<Integer>>>
inline constexpr auto FromRatio(Integer numerator, Integer denominator) {
  return DivisionExpression<Integer, Integer>{numerator, denominator};
}

// Utility to coerce an expression to the given resolution.
template <size_t FractionalBits, typename T>
inline constexpr auto ToResolution(T expression) {
  return ResolutionExpression<FractionalBits, T>{Init{}, expression};
}

// Utility to create a value Expression from a raw integer value already in the
// fixed-point format with the given number of fractional bits.
template <size_t FractionalBits, typename Integer>
inline constexpr auto FromRaw(Integer value) {
  return ValueExpression<Integer, FractionalBits>{value};
}

// Relational operators. Note that relational operators convert to the format
// with the least precision before comparison. This means that comparing with
// an integer directly is different than comparing with an integer converted to
// the same fixed-point type, due to rounding in the former.
//
// For example,
//
//  constexpr Fixed<int32_t, 1> value{FromRatio(1, 2)};
//  constexpr bool compare_a = value > 0;
//  constexpr bool compare_b = value > Fixed<int32_t, 1>{0};
//
//  static_assert(compare_a != compare_b, "");
//
// In the former case, compare_a expresses whether the value rounds to greater
// than zero. Whereas, in the latter case, compare_b expresses whether the value
// is greater than zero, even fractionally. Because this library uses convergent
// rounding these comparisons do not always yield the same result.
//
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator<(Left left, Right right) {
  using Traits = ComparisonTraits<Left, Right>;
  return Traits::Left(left) < Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator>(Left left, Right right) {
  using Traits = ComparisonTraits<Left, Right>;
  return Traits::Left(left) > Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator<=(Left left, Right right) {
  using Traits = ComparisonTraits<Left, Right>;
  return Traits::Left(left) <= Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator>=(Left left, Right right) {
  using Traits = ComparisonTraits<Left, Right>;
  return Traits::Left(left) >= Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator==(Left left, Right right) {
  using Traits = ComparisonTraits<Left, Right>;
  return Traits::Left(left) == Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator!=(Left left, Right right) {
  using Traits = ComparisonTraits<Left, Right>;
  return Traits::Left(left) != Traits::Right(right);
}

// Arithmetic operators. These operators accept any combination of Fixed,
// integer, and Expression (excluding integer/integer which is handled by the
// language). The return type and value captures the operation and operands as
// an Expression for later evaluation. Evaluation is performed when the
// Expression tree is assigned to a Fixed variable. This can be composed in
// multiple stages and assignments.
//
// Example:
//
//     const int32_t value = ...;
//     cosnt int32_t offset = ...;
//
//     const auto quotient = FromRatio(value, 3);
//     const Fixed<int32_t, 1> low_precision = quotient;
//     const Fixed<int64_t, 10> high_precision = quotient;
//
//     const auto with_offset = quotient + ToResolution<10>(offset);
//     const Fixed<int64_t, 10> high_precision_with_offset = with_offset;
//
template <typename Left, typename Right, typename Enabled = EnableIfBinaryExpression<Left, Right>>
inline constexpr auto operator+(Left left, Right right) {
  return AdditionExpression<Left, Right>{left, right};
}
template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
inline constexpr auto operator-(T value) {
  return NegationExpression<T>{Init{}, value};
}
template <typename Left, typename Right, typename Enabled = EnableIfBinaryExpression<Left, Right>>
inline constexpr auto operator-(Left left, Right right) {
  return SubtractionExpression<Left, Right>{left, right};
}
template <typename Left, typename Right, typename Enabled = EnableIfBinaryExpression<Left, Right>>
inline constexpr auto operator*(Left left, Right right) {
  return MultiplicationExpression<Left, Right>{left, right};
}
template <typename Left, typename Right, typename Enabled = EnableIfBinaryExpression<Left, Right>>
inline constexpr auto operator/(Left left, Right right) {
  return DivisionExpression<Left, Right>{left, right};
}

}  // namespace ffl
