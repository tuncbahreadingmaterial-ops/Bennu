#include "bennu/value.hpp"

#include "doctest/doctest.h"

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bennu {

namespace {

constexpr std::uint64_t binary64_exponent_mask = UINT64_C(0x7ff0000000000000);
constexpr std::uint64_t binary64_fraction_mask = UINT64_C(0x000fffffffffffff);
constexpr std::uint64_t canonical_nan_bits = UINT64_C(0x7ff8000000000000);
static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<double>::is_iec559);

double normalize_double(double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & binary64_exponent_mask) == binary64_exponent_mask &&
      (bits & binary64_fraction_mask) != 0) {
    return std::bit_cast<double>(canonical_nan_bits);
  }
  return value;
}

bool is_positive_zero(double value) {
  return std::bit_cast<std::uint64_t>(value) == UINT64_C(0);
}

bool is_canonical_double(double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & binary64_exponent_mask) != binary64_exponent_mask ||
      (bits & binary64_fraction_mask) == 0) {
    return true;
  }
  return bits == canonical_nan_bits;
}

ScalarValue empty_scalar() {
  return ScalarValue{ScalarType::boolean, false, 0, 0.0};
}

VectorValue empty_vector() {
  return VectorValue{ScalarType::boolean, {}, {}, {}};
}

Value invalid_construction_value() {
  return Value{ContainerKind::scalar, empty_scalar(), empty_vector()};
}

ValueValidationResult validate_scalar(const ScalarValue &scalar) {
  switch (scalar.type) {
  case ScalarType::boolean:
    if (scalar.integer != 0 || !is_positive_zero(scalar.double_precision)) {
      return ValueValidationResult{false,
                                   ValueInvariant::inactive_scalar_field};
    }
    return ValueValidationResult{true, ValueInvariant::none};
  case ScalarType::integer:
    if (scalar.boolean || !is_positive_zero(scalar.double_precision)) {
      return ValueValidationResult{false,
                                   ValueInvariant::inactive_scalar_field};
    }
    return ValueValidationResult{true, ValueInvariant::none};
  case ScalarType::double_precision:
    if (scalar.boolean || scalar.integer != 0) {
      return ValueValidationResult{false,
                                   ValueInvariant::inactive_scalar_field};
    }
    if (!is_canonical_double(scalar.double_precision)) {
      return ValueValidationResult{false, ValueInvariant::noncanonical_nan};
    }
    return ValueValidationResult{true, ValueInvariant::none};
  }
  return ValueValidationResult{false, ValueInvariant::unknown_scalar_type};
}

bool is_empty_scalar(const ScalarValue &scalar) {
  return scalar.type == ScalarType::boolean && !scalar.boolean &&
         scalar.integer == 0 && is_positive_zero(scalar.double_precision);
}

ValueConstructionResult construction_failure(ValueInvariant invariant) {
  return ValueConstructionResult{false, invalid_construction_value(), invariant};
}

std::size_t active_vector_length(const VectorValue &vector) {
  switch (vector.element_type) {
  case ScalarType::boolean:
    return vector.booleans.size();
  case ScalarType::integer:
    return vector.integers.size();
  case ScalarType::double_precision:
    return vector.doubles.size();
  }
  return 0;
}

std::string format_double(double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & binary64_exponent_mask) == binary64_exponent_mask) {
    if ((bits & binary64_fraction_mask) != 0) {
      return "nan";
    }
    return (bits >> 63U) != 0 ? "-inf" : "inf";
  }
  if (bits == UINT64_C(0)) {
    return "0.0";
  }
  if (bits == UINT64_C(0x8000000000000000)) {
    return "-0.0";
  }

  std::array<char, 64> buffer{};
  const std::to_chars_result converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (converted.ec != std::errc{}) {
    return {};
  }
  std::string formatted(buffer.data(), converted.ptr);
  const std::size_t exponent = formatted.find_first_of("eE");
  if (exponent == std::string::npos) {
    if (formatted.find('.') == std::string::npos) {
      formatted += ".0";
    }
    return formatted;
  }

  formatted[exponent] = 'e';
  std::size_t digits = exponent + 1;
  bool negative = false;
  if (formatted[digits] == '+' || formatted[digits] == '-') {
    negative = formatted[digits] == '-';
    ++digits;
  }
  while (digits + 1 < formatted.size() && formatted[digits] == '0') {
    ++digits;
  }
  std::string normalized = formatted.substr(0, exponent + 1);
  if (negative) {
    normalized += '-';
  }
  normalized += formatted.substr(digits);
  return normalized;
}

void append_scalar(std::string &formatted, const ScalarValue &scalar) {
  switch (scalar.type) {
  case ScalarType::boolean:
    formatted += scalar.boolean ? "true" : "false";
    return;
  case ScalarType::integer:
    formatted += std::to_string(scalar.integer);
    return;
  case ScalarType::double_precision:
    formatted += format_double(scalar.double_precision);
    return;
  }
}

} // namespace

Value make_bool_value(bool value) {
  return Value{ContainerKind::scalar,
               ScalarValue{ScalarType::boolean, value, 0, 0.0},
               empty_vector()};
}

Value make_int_value(std::int64_t value) {
  return Value{ContainerKind::scalar,
               ScalarValue{ScalarType::integer, false, value, 0.0},
               empty_vector()};
}

Value make_double_value(double value) {
  return Value{ContainerKind::scalar,
               ScalarValue{ScalarType::double_precision, false, 0,
                           normalize_double(value)},
               empty_vector()};
}

ValueConstructionResult make_bool_vector(std::vector<std::uint8_t> values) {
  for (const std::uint8_t value : values) {
    if (value > 1U) {
      return construction_failure(ValueInvariant::invalid_boolean_element);
    }
  }
  return ValueConstructionResult{
      true,
      Value{ContainerKind::vector,
            empty_scalar(),
            VectorValue{ScalarType::boolean, std::move(values), {}, {}}},
      ValueInvariant::none,
  };
}

ValueConstructionResult make_int_vector(std::vector<std::int64_t> values) {
  return ValueConstructionResult{
      true,
      Value{ContainerKind::vector,
            empty_scalar(),
            VectorValue{ScalarType::integer, {}, std::move(values), {}}},
      ValueInvariant::none,
  };
}

ValueConstructionResult make_double_vector(std::vector<double> values) {
  for (double &value : values) {
    value = normalize_double(value);
  }
  return ValueConstructionResult{
      true,
      Value{ContainerKind::vector,
            empty_scalar(),
            VectorValue{ScalarType::double_precision, {}, {}, std::move(values)}},
      ValueInvariant::none,
  };
}

ValueValidationResult validate_value(const Value &value) {
  switch (value.container) {
  case ContainerKind::scalar:
    if (value.vector.element_type != ScalarType::boolean ||
        !value.vector.booleans.empty() || !value.vector.integers.empty() ||
        !value.vector.doubles.empty()) {
      return ValueValidationResult{false,
                                   ValueInvariant::inactive_vector_payload};
    }
    return validate_scalar(value.scalar);
  case ContainerKind::vector:
    if (!is_empty_scalar(value.scalar)) {
      return ValueValidationResult{false,
                                   ValueInvariant::inactive_scalar_field};
    }
    switch (value.vector.element_type) {
    case ScalarType::boolean:
      if (!value.vector.integers.empty() || !value.vector.doubles.empty()) {
        return ValueValidationResult{
            false, ValueInvariant::inactive_vector_payload};
      }
      for (const std::uint8_t element : value.vector.booleans) {
        if (element > 1U) {
          return ValueValidationResult{
              false, ValueInvariant::invalid_boolean_element};
        }
      }
      return ValueValidationResult{true, ValueInvariant::none};
    case ScalarType::integer:
      if (!value.vector.booleans.empty() || !value.vector.doubles.empty()) {
        return ValueValidationResult{
            false, ValueInvariant::inactive_vector_payload};
      }
      return ValueValidationResult{true, ValueInvariant::none};
    case ScalarType::double_precision:
      if (!value.vector.booleans.empty() || !value.vector.integers.empty()) {
        return ValueValidationResult{
            false, ValueInvariant::inactive_vector_payload};
      }
      for (const double element : value.vector.doubles) {
        if (!is_canonical_double(element)) {
          return ValueValidationResult{false,
                                       ValueInvariant::noncanonical_nan};
        }
      }
      return ValueValidationResult{true, ValueInvariant::none};
    }
    return ValueValidationResult{false, ValueInvariant::unknown_scalar_type};
  }
  return ValueValidationResult{false, ValueInvariant::unknown_container};
}

ScalarType value_element_type(const Value &value) {
  return value.container == ContainerKind::scalar ? value.scalar.type
                                                  : value.vector.element_type;
}

std::size_t value_rank(const Value &value) {
  return value.container == ContainerKind::scalar ? 0 : 1;
}

std::size_t value_length(const Value &value) {
  return value.container == ContainerKind::scalar
             ? 1
             : active_vector_length(value.vector);
}

ScalarProjectionResult project_scalar(const Value &value, std::size_t index) {
  if (value.container == ContainerKind::scalar) {
    return ScalarProjectionResult{true, value.scalar, ValueAccessError::none};
  }
  if (value.container != ContainerKind::vector) {
    return ScalarProjectionResult{false, empty_scalar(),
                                  ValueAccessError::invalid_value};
  }
  switch (value.vector.element_type) {
  case ScalarType::boolean:
    if (index >= value.vector.booleans.size()) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::boolean, value.vector.booleans[index] != 0, 0,
                    0.0},
        ValueAccessError::none};
  case ScalarType::integer:
    if (index >= value.vector.integers.size()) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::integer, false, value.vector.integers[index],
                    0.0},
        ValueAccessError::none};
  case ScalarType::double_precision:
    if (index >= value.vector.doubles.size()) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::double_precision, false, 0,
                    value.vector.doubles[index]},
        ValueAccessError::none};
  }
  return ScalarProjectionResult{false, empty_scalar(),
                                ValueAccessError::invalid_value};
}

void destroy_value(Value &value) {
  value = invalid_construction_value();
}

std::string format_value(const Value &value) {
  if (value.container == ContainerKind::scalar) {
    std::string formatted;
    append_scalar(formatted, value.scalar);
    return formatted;
  }

  std::string formatted = "(";
  const std::size_t length = active_vector_length(value.vector);
  for (std::size_t index = 0; index < length; ++index) {
    if (index != 0) {
      formatted += ' ';
    }
    switch (value.vector.element_type) {
    case ScalarType::boolean:
      formatted += value.vector.booleans[index] != 0 ? "true" : "false";
      break;
    case ScalarType::integer:
      formatted += std::to_string(value.vector.integers[index]);
      break;
    case ScalarType::double_precision:
      formatted += format_double(value.vector.doubles[index]);
      break;
    }
  }
  formatted += ')';
  return formatted;
}

TEST_CASE("typed scalar construction produces valid direct tagged values") {
  const Value boolean = make_bool_value(true);
  const Value integer = make_int_value(INT64_MIN);
  const Value double_precision = make_double_value(-0.0);

  CHECK(validate_value(boolean).ok);
  CHECK(boolean.container == ContainerKind::scalar);
  CHECK(boolean.scalar.type == ScalarType::boolean);
  CHECK(boolean.scalar.boolean);

  CHECK(validate_value(integer).ok);
  CHECK(integer.scalar.type == ScalarType::integer);
  CHECK(integer.scalar.integer == INT64_MIN);

  CHECK(validate_value(double_precision).ok);
  CHECK(double_precision.scalar.type == ScalarType::double_precision);
  CHECK(double_precision.scalar.double_precision == 0.0);
}

TEST_CASE("vectors keep one untagged typed payload and preserve empty types") {
  static_assert(std::is_same_v<decltype(VectorValue::booleans)::value_type,
                               std::uint8_t>);
  static_assert(std::is_same_v<decltype(VectorValue::integers)::value_type,
                               std::int64_t>);
  static_assert(
      std::is_same_v<decltype(VectorValue::doubles)::value_type, double>);

  const ValueConstructionResult booleans = make_bool_vector({});
  const ValueConstructionResult integers = make_int_vector({});
  const ValueConstructionResult doubles = make_double_vector({});

  REQUIRE(booleans.ok);
  REQUIRE(integers.ok);
  REQUIRE(doubles.ok);
  CHECK(booleans.value.vector.element_type == ScalarType::boolean);
  CHECK(integers.value.vector.element_type == ScalarType::integer);
  CHECK(doubles.value.vector.element_type == ScalarType::double_precision);
  CHECK(value_length(booleans.value) == 0);
  CHECK(value_length(integers.value) == 0);
  CHECK(value_length(doubles.value) == 0);
  CHECK(format_value(booleans.value) == "()");
  CHECK(format_value(integers.value) == "()");
  CHECK(format_value(doubles.value) == "()");
}

TEST_CASE("construction and validation reject invalid homogeneous payloads") {
  const ValueConstructionResult invalid_boolean = make_bool_vector({0, 2, 1});
  CHECK_FALSE(invalid_boolean.ok);
  CHECK(invalid_boolean.invariant == ValueInvariant::invalid_boolean_element);

  Value inactive = make_int_vector({1, 2}).value;
  inactive.vector.doubles.push_back(2.0);
  const ValueValidationResult inactive_result = validate_value(inactive);
  CHECK_FALSE(inactive_result.ok);
  CHECK(inactive_result.invariant == ValueInvariant::inactive_vector_payload);

  Value inactive_tag = make_bool_value(false);
  inactive_tag.vector.element_type = ScalarType::integer;
  CHECK_FALSE(validate_value(inactive_tag).ok);

  Value inactive_scalar = make_int_vector({1}).value;
  inactive_scalar.scalar.integer = 9;
  const ValueValidationResult inactive_scalar_result =
      validate_value(inactive_scalar);
  CHECK_FALSE(inactive_scalar_result.ok);
  CHECK(inactive_scalar_result.invariant ==
        ValueInvariant::inactive_scalar_field);

  Value noncanonical = make_double_vector({1.0}).value;
  noncanonical.vector.doubles[0] =
      std::bit_cast<double>(UINT64_C(0xfff8123456789abc));
  const ValueValidationResult nan_result = validate_value(noncanonical);
  CHECK_FALSE(nan_result.ok);
  CHECK(nan_result.invariant == ValueInvariant::noncanonical_nan);

  const ValueConstructionResult normalized = make_double_vector(
      {std::bit_cast<double>(UINT64_C(0xfff8123456789abc))});
  REQUIRE(normalized.ok);
  CHECK(std::bit_cast<std::uint64_t>(normalized.value.vector.doubles[0]) ==
        canonical_nan_bits);
}

TEST_CASE("rank length and projection follow scalar and vector identity") {
  const Value scalar = make_int_value(42);
  const Value vector = make_int_vector({4, 5, 6}).value;

  CHECK(value_rank(scalar) == 0);
  CHECK(value_length(scalar) == 1);
  CHECK(value_element_type(scalar) == ScalarType::integer);
  const ScalarProjectionResult broadcast = project_scalar(scalar, 12);
  REQUIRE(broadcast.ok);
  CHECK(broadcast.value.integer == 42);

  CHECK(value_rank(vector) == 1);
  CHECK(value_length(vector) == 3);
  CHECK(value_element_type(vector) == ScalarType::integer);
  const ScalarProjectionResult projected = project_scalar(vector, 1);
  REQUIRE(projected.ok);
  CHECK(projected.value.type == ScalarType::integer);
  CHECK(projected.value.integer == 5);
  const ScalarProjectionResult outside = project_scalar(vector, 3);
  CHECK_FALSE(outside.ok);
  CHECK(outside.error == ValueAccessError::index_out_of_bounds);
}

TEST_CASE("canonical scalar and vector formatting is byte exact") {
  CHECK(format_value(make_bool_value(false)) == "false");
  CHECK(format_value(make_bool_value(true)) == "true");
  CHECK(format_value(make_int_value(INT64_MIN)) == "-9223372036854775808");
  CHECK(format_value(make_int_value(INT64_MAX)) == "9223372036854775807");
  CHECK(format_value(make_double_value(1.0)) == "1.0");
  CHECK(format_value(make_double_value(-42.0)) == "-42.0");
  CHECK(format_value(make_double_value(1.0e20)) == "1e20");
  CHECK(format_value(make_double_value(1.0e-7)) == "1e-7");
  CHECK(format_value(make_double_value(-0.0)) == "-0.0");
  CHECK(format_value(make_double_value(std::numeric_limits<double>::infinity())) ==
        "inf");
  CHECK(format_value(make_double_value(
            -std::numeric_limits<double>::infinity())) == "-inf");
  CHECK(format_value(make_double_value(
            std::numeric_limits<double>::quiet_NaN())) == "nan");

  CHECK(format_value(make_bool_vector({0, 1, 0}).value) ==
        "(false true false)");
  CHECK(format_value(make_int_vector({1, -2, 3}).value) == "(1 -2 3)");
  CHECK(format_value(make_double_vector({1.0, 2.5, 3.0}).value) ==
        "(1.0 2.5 3.0)");
  CHECK(format_value(make_double_vector(
                         {std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity(), -0.0})
                         .value) == "(nan inf -inf -0.0)");
}

TEST_CASE("binary64 formatting round trips boundaries and normalizes spelling") {
  struct FormatCase {
    std::uint64_t bits;
    std::string_view expected;
  };
  const std::array<FormatCase, 7> cases{{
      {UINT64_C(0x0000000000000001), "5e-324"},
      {UINT64_C(0x000fffffffffffff), "2.225073858507201e-308"},
      {UINT64_C(0x0010000000000000), "2.2250738585072014e-308"},
      {UINT64_C(0x7fefffffffffffff), "1.7976931348623157e308"},
      {UINT64_C(0x3ff0000000000000), "1.0"},
      {UINT64_C(0x8000000000000000), "-0.0"},
      {UINT64_C(0x7ff0000000000000), "inf"},
  }};
  for (const FormatCase &format_case : cases) {
    CAPTURE(format_case.expected);
    const double value = std::bit_cast<double>(format_case.bits);
    const std::string formatted = format_value(make_double_value(value));
    CHECK(formatted == format_case.expected);
    if ((format_case.bits & binary64_exponent_mask) !=
        binary64_exponent_mask) {
      double parsed = 0.0;
      const std::from_chars_result round_trip = std::from_chars(
          formatted.data(), formatted.data() + formatted.size(), parsed,
          std::chars_format::general);
      CHECK(round_trip.ec == std::errc{});
      CHECK(round_trip.ptr == formatted.data() + formatted.size());
      CHECK(std::bit_cast<std::uint64_t>(parsed) == format_case.bits);
    }
  }

  const Value normalized = make_double_value(
      std::bit_cast<double>(UINT64_C(0xfff0000000000001)));
  CHECK(std::bit_cast<std::uint64_t>(normalized.scalar.double_precision) ==
        UINT64_C(0x7ff8000000000000));
  CHECK(format_value(normalized) == "nan");
}

TEST_CASE("explicit destruction releases owned payload and leaves a valid value") {
  Value value = make_double_vector({1.0, 2.0, 3.0}).value;
  REQUIRE(value.vector.doubles.size() == 3);

  destroy_value(value);

  CHECK(validate_value(value).ok);
  CHECK(value.container == ContainerKind::scalar);
  CHECK(value.scalar.type == ScalarType::boolean);
  CHECK_FALSE(value.scalar.boolean);
  CHECK(value.vector.booleans.empty());
  CHECK(value.vector.integers.empty());
  CHECK(value.vector.doubles.empty());
}

} // namespace bennu
