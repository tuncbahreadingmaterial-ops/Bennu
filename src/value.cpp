#include "bennu/value.hpp"
#include "bennu/resources.hpp"

#include "doctest/doctest.h"

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <span>
#include <string>
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
  return VectorValue{ScalarType::boolean,
                     {nullptr, &std::free},
                     0,
                     {nullptr, &std::free},
                     0,
                     {nullptr, &std::free},
                     0};
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

std::size_t active_vector_length(const VectorValue &vector) {
  switch (vector.element_type) {
  case ScalarType::boolean:
    return vector.boolean_count;
  case ScalarType::integer:
    return vector.integer_count;
  case ScalarType::double_precision:
    return vector.double_count;
  }
  return 0;
}

bool append_double(std::string &formatted, double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & binary64_exponent_mask) == binary64_exponent_mask) {
    if ((bits & binary64_fraction_mask) != 0) {
      formatted += "nan";
      return true;
    }
    formatted += (bits >> 63U) != 0 ? "-inf" : "inf";
    return true;
  }
  if (bits == UINT64_C(0)) {
    formatted += "0.0";
    return true;
  }
  if (bits == UINT64_C(0x8000000000000000)) {
    formatted += "-0.0";
    return true;
  }

  std::array<char, 64> buffer{};
  const std::to_chars_result converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (converted.ec != std::errc{}) {
    return false;
  }
  const char *exponent = buffer.data();
  while (exponent != converted.ptr && *exponent != 'e' && *exponent != 'E') {
    ++exponent;
  }
  if (exponent == converted.ptr) {
    const char *point = buffer.data();
    while (point != converted.ptr && *point != '.') {
      ++point;
    }
    formatted.append(buffer.data(), converted.ptr);
    if (point == converted.ptr) {
      formatted += ".0";
    }
    return true;
  }

  formatted.append(buffer.data(),
                   static_cast<std::size_t>(exponent - buffer.data()));
  formatted += 'e';
  const char *digits = exponent + 1;
  bool negative = false;
  if (digits != converted.ptr && (*digits == '+' || *digits == '-')) {
    negative = *digits == '-';
    ++digits;
  }
  while (digits + 1 < converted.ptr && *digits == '0') {
    ++digits;
  }
  if (negative) {
    formatted += '-';
  }
  formatted.append(digits,
                   static_cast<std::size_t>(converted.ptr - digits));
  return true;
}

bool append_integer(std::string &formatted, std::int64_t value) {
  std::array<char, 32> buffer{};
  const std::to_chars_result converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (converted.ec != std::errc{}) {
    return false;
  }
  formatted.append(buffer.data(), converted.ptr);
  return true;
}

bool append_scalar(std::string &formatted, const ScalarValue &scalar) {
  switch (scalar.type) {
  case ScalarType::boolean:
    formatted += scalar.boolean ? "true" : "false";
    return true;
  case ScalarType::integer:
    return append_integer(formatted, scalar.integer);
  case ScalarType::double_precision:
    return append_double(formatted, scalar.double_precision);
  }
  return false;
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

ValueValidationResult validate_value(const Value &value) {
  switch (value.container) {
  case ContainerKind::scalar:
    if (value.vector.element_type != ScalarType::boolean ||
        value.vector.booleans.get() != nullptr ||
        value.vector.boolean_count != 0 ||
        value.vector.integers.get() != nullptr ||
        value.vector.integer_count != 0 ||
        value.vector.doubles.get() != nullptr ||
        value.vector.double_count != 0) {
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
      if (value.vector.integers.get() != nullptr ||
          value.vector.integer_count != 0 ||
          value.vector.doubles.get() != nullptr ||
          value.vector.double_count != 0 ||
          (value.vector.boolean_count != 0 &&
           value.vector.booleans.get() == nullptr)) {
        return ValueValidationResult{
            false, ValueInvariant::inactive_vector_payload};
      }
      for (std::size_t index = 0; index < value.vector.boolean_count; ++index) {
        const std::uint8_t element = value.vector.booleans.get()[index];
        if (element > 1U) {
          return ValueValidationResult{
              false, ValueInvariant::invalid_boolean_element};
        }
      }
      return ValueValidationResult{true, ValueInvariant::none};
    case ScalarType::integer:
      if (value.vector.booleans.get() != nullptr ||
          value.vector.boolean_count != 0 ||
          value.vector.doubles.get() != nullptr ||
          value.vector.double_count != 0 ||
          (value.vector.integer_count != 0 &&
           value.vector.integers.get() == nullptr)) {
        return ValueValidationResult{
            false, ValueInvariant::inactive_vector_payload};
      }
      return ValueValidationResult{true, ValueInvariant::none};
    case ScalarType::double_precision:
      if (value.vector.booleans.get() != nullptr ||
          value.vector.boolean_count != 0 ||
          value.vector.integers.get() != nullptr ||
          value.vector.integer_count != 0 ||
          (value.vector.double_count != 0 &&
           value.vector.doubles.get() == nullptr)) {
        return ValueValidationResult{
            false, ValueInvariant::inactive_vector_payload};
      }
      for (std::size_t index = 0; index < value.vector.double_count; ++index) {
        const double element = value.vector.doubles.get()[index];
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

ValueValidationResult value_element_type(const Value &value,
                                         ScalarType &element_type) {
  element_type = ScalarType::boolean;
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    return validation;
  }
  element_type = value.container == ContainerKind::scalar
                     ? value.scalar.type
                     : value.vector.element_type;
  return validation;
}

ValueValidationResult value_rank(const Value &value, std::size_t &rank) {
  rank = 0;
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    return validation;
  }
  rank = value.container == ContainerKind::scalar ? 0 : 1;
  return validation;
}

ValueValidationResult value_length(const Value &value, std::size_t &length) {
  length = 0;
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    return validation;
  }
  length = value.container == ContainerKind::scalar
               ? 1
               : active_vector_length(value.vector);
  return validation;
}

ScalarProjectionResult project_scalar(const Value &value, std::size_t index) {
  if (!validate_value(value).ok) {
    return ScalarProjectionResult{false, empty_scalar(),
                                  ValueAccessError::invalid_value};
  }
  if (value.container == ContainerKind::scalar) {
    return ScalarProjectionResult{true, value.scalar, ValueAccessError::none};
  }
  switch (value.vector.element_type) {
  case ScalarType::boolean:
    if (index >= value.vector.boolean_count) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::boolean,
                    value.vector.booleans.get()[index] != 0, 0,
                    0.0},
        ValueAccessError::none};
  case ScalarType::integer:
    if (index >= value.vector.integer_count) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::integer, false,
                    value.vector.integers.get()[index],
                    0.0},
        ValueAccessError::none};
  case ScalarType::double_precision:
    if (index >= value.vector.double_count) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::double_precision, false, 0,
                    value.vector.doubles.get()[index]},
        ValueAccessError::none};
  }
  return ScalarProjectionResult{false, empty_scalar(),
                                ValueAccessError::invalid_value};
}

void destroy_value(Value &value) {
  value = invalid_construction_value();
}

ValueFormattingResult format_value(const Value &value) {
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    return ValueFormattingResult{false, {}, validation.invariant,
                                 ValueFormatError::invalid_value};
  }

  std::string formatted;
  if (value.container == ContainerKind::scalar) {
    if (!append_scalar(formatted, value.scalar)) {
      return ValueFormattingResult{false, {}, ValueInvariant::none,
                                   ValueFormatError::conversion_failure};
    }
    return ValueFormattingResult{true, std::move(formatted),
                                 ValueInvariant::none, ValueFormatError::none};
  }

  const std::size_t length = active_vector_length(value.vector);
  constexpr std::size_t maximum_element_bytes = 25;
  if (length <=
      (formatted.max_size() - std::size_t{2}) / maximum_element_bytes) {
    formatted.reserve(std::size_t{2} + length * maximum_element_bytes);
  }
  formatted += '(';
  for (std::size_t index = 0; index < length; ++index) {
    if (index != 0) {
      formatted += ' ';
    }
    switch (value.vector.element_type) {
    case ScalarType::boolean:
      formatted +=
          value.vector.booleans.get()[index] != 0 ? "true" : "false";
      break;
    case ScalarType::integer:
      if (!append_integer(formatted, value.vector.integers.get()[index])) {
        return ValueFormattingResult{false, {}, ValueInvariant::none,
                                     ValueFormatError::conversion_failure};
      }
      break;
    case ScalarType::double_precision:
      if (!append_double(formatted, value.vector.doubles.get()[index])) {
        return ValueFormattingResult{false, {}, ValueInvariant::none,
                                     ValueFormatError::conversion_failure};
      }
      break;
    }
  }
  formatted += ')';
  return ValueFormattingResult{true, std::move(formatted), ValueInvariant::none,
                               ValueFormatError::none};
}

namespace {

using ValueConstructionResult = VectorAllocationResult;

ValueConstructionResult
make_bool_vector(std::initializer_list<std::uint8_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  return copy_bool_vector(
      resources, std::span<const std::uint8_t>(values.begin(), values.size()),
      SourceLocation{0, 1, 1}, "value-construction");
}

ValueConstructionResult
make_int_vector(std::initializer_list<std::int64_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  return copy_int_vector(
      resources, std::span<const std::int64_t>(values.begin(), values.size()),
      SourceLocation{0, 1, 1}, "value-construction");
}

ValueConstructionResult make_double_vector(std::initializer_list<double> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  return copy_double_vector(
      resources, std::span<const double>(values.begin(), values.size()),
      SourceLocation{0, 1, 1}, "value-construction");
}

[[maybe_unused]] std::string format_valid_test_value(const Value &value) {
  ValueFormattingResult result = format_value(value);
  CHECK(result.ok);
  return std::move(result.formatted);
}

[[maybe_unused]] ScalarType valid_element_type_for_test(const Value &value) {
  (void)value;
  ScalarType element_type = ScalarType::boolean;
  CHECK(value_element_type(value, element_type).ok);
  return element_type;
}

[[maybe_unused]] std::size_t valid_rank_for_test(const Value &value) {
  (void)value;
  std::size_t rank = 0;
  CHECK(value_rank(value, rank).ok);
  return rank;
}

[[maybe_unused]] std::size_t valid_length_for_test(const Value &value) {
  (void)value;
  std::size_t length = 0;
  CHECK(value_length(value, length).ok);
  return length;
}

} // namespace

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
  static_assert(std::is_same_v<decltype(VectorValue::booleans)::element_type,
                               std::uint8_t>);
  static_assert(std::is_same_v<decltype(VectorValue::integers)::element_type,
                               std::int64_t>);
  static_assert(
      std::is_same_v<decltype(VectorValue::doubles)::element_type, double>);

  const ValueConstructionResult booleans = make_bool_vector({});
  const ValueConstructionResult integers = make_int_vector({});
  const ValueConstructionResult doubles = make_double_vector({});

  REQUIRE(booleans.ok);
  REQUIRE(integers.ok);
  REQUIRE(doubles.ok);
  CHECK(booleans.value.vector.element_type == ScalarType::boolean);
  CHECK(integers.value.vector.element_type == ScalarType::integer);
  CHECK(doubles.value.vector.element_type == ScalarType::double_precision);
  CHECK(valid_length_for_test(booleans.value) == 0);
  CHECK(valid_length_for_test(integers.value) == 0);
  CHECK(valid_length_for_test(doubles.value) == 0);
  CHECK(format_valid_test_value(booleans.value) == "()");
  CHECK(format_valid_test_value(integers.value) == "()");
  CHECK(format_valid_test_value(doubles.value) == "()");
}

TEST_CASE("construction and validation reject invalid homogeneous payloads") {
  const ValueConstructionResult invalid_boolean = make_bool_vector({0, 2, 1});
  CHECK_FALSE(invalid_boolean.ok);
  CHECK(invalid_boolean.invariant == ValueInvariant::invalid_boolean_element);

  Value inactive = make_int_vector({1, 2}).value;
  Value unexpected_double = make_double_vector({2.0}).value;
  inactive.vector.doubles = std::move(unexpected_double.vector.doubles);
  inactive.vector.double_count = 1;
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
  noncanonical.vector.doubles.get()[0] =
      std::bit_cast<double>(UINT64_C(0xfff8123456789abc));
  const ValueValidationResult nan_result = validate_value(noncanonical);
  CHECK_FALSE(nan_result.ok);
  CHECK(nan_result.invariant == ValueInvariant::noncanonical_nan);

  const ValueConstructionResult normalized = make_double_vector(
      {std::bit_cast<double>(UINT64_C(0xfff8123456789abc))});
  REQUIRE(normalized.ok);
  CHECK(std::bit_cast<std::uint64_t>(
            normalized.value.vector.doubles.get()[0]) ==
        canonical_nan_bits);
}

TEST_CASE("rank length and projection follow scalar and vector identity") {
  const Value scalar = make_int_value(42);
  const Value vector = make_int_vector({4, 5, 6}).value;

  CHECK(valid_rank_for_test(scalar) == 0);
  CHECK(valid_length_for_test(scalar) == 1);
  CHECK(valid_element_type_for_test(scalar) == ScalarType::integer);
  const ScalarProjectionResult broadcast = project_scalar(scalar, 12);
  REQUIRE(broadcast.ok);
  CHECK(broadcast.value.integer == 42);

  CHECK(valid_rank_for_test(vector) == 1);
  CHECK(valid_length_for_test(vector) == 3);
  CHECK(valid_element_type_for_test(vector) == ScalarType::integer);
  const ScalarProjectionResult projected = project_scalar(vector, 1);
  REQUIRE(projected.ok);
  CHECK(projected.value.type == ScalarType::integer);
  CHECK(projected.value.integer == 5);
  const ScalarProjectionResult outside = project_scalar(vector, 3);
  CHECK_FALSE(outside.ok);
  CHECK(outside.error == ValueAccessError::index_out_of_bounds);
}

TEST_CASE("canonical scalar and vector formatting is byte exact") {
  CHECK(format_valid_test_value(make_bool_value(false)) == "false");
  CHECK(format_valid_test_value(make_bool_value(true)) == "true");
  CHECK(format_valid_test_value(make_int_value(INT64_MIN)) ==
        "-9223372036854775808");
  CHECK(format_valid_test_value(make_int_value(INT64_MAX)) ==
        "9223372036854775807");
  CHECK(format_valid_test_value(make_double_value(1.0)) == "1.0");
  CHECK(format_valid_test_value(make_double_value(-42.0)) == "-42.0");
  CHECK(format_valid_test_value(make_double_value(1.0e20)) == "1e20");
  CHECK(format_valid_test_value(make_double_value(1.0e-7)) == "1e-7");
  CHECK(format_valid_test_value(make_double_value(-0.0)) == "-0.0");
  CHECK(format_valid_test_value(
            make_double_value(std::numeric_limits<double>::infinity())) ==
        "inf");
  CHECK(format_valid_test_value(make_double_value(
            -std::numeric_limits<double>::infinity())) == "-inf");
  CHECK(format_valid_test_value(make_double_value(
            std::numeric_limits<double>::quiet_NaN())) == "nan");

  CHECK(format_valid_test_value(make_bool_vector({0, 1, 0}).value) ==
        "(false true false)");
  CHECK(format_valid_test_value(make_int_vector({1, -2, 3}).value) ==
        "(1 -2 3)");
  CHECK(format_valid_test_value(make_double_vector({1.0, 2.5, 3.0}).value) ==
        "(1.0 2.5 3.0)");
  CHECK(format_valid_test_value(
            make_double_vector({std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(), -0.0})
                .value) == "(nan inf -inf -0.0)");
}

TEST_CASE("binary64 formatting round trips boundaries and normalizes spelling") {
  struct FormatCase {
    std::uint64_t bits;
    const char *expected;
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
    const std::string formatted =
        format_valid_test_value(make_double_value(value));
    CHECK(formatted == format_case.expected);
    if ((format_case.bits & binary64_exponent_mask) !=
        binary64_exponent_mask) {
      char *round_trip_end = nullptr;
      const double parsed = std::strtod(formatted.c_str(), &round_trip_end);
      CHECK(round_trip_end == formatted.c_str() + formatted.size());
      CHECK(std::bit_cast<std::uint64_t>(parsed) == format_case.bits);
    }
  }

  const Value normalized = make_double_value(
      std::bit_cast<double>(UINT64_C(0xfff0000000000001)));
  CHECK(std::bit_cast<std::uint64_t>(normalized.scalar.double_precision) ==
        UINT64_C(0x7ff8000000000000));
  CHECK(format_valid_test_value(normalized) == "nan");
}

TEST_CASE("public value consumers reject malformed plain records") {
  Value unknown_container = make_int_vector({1}).value;
  unknown_container.container = static_cast<ContainerKind>(99);
  Value unknown_scalar = make_bool_value(false);
  unknown_scalar.scalar.type = static_cast<ScalarType>(99);
  Value inactive_payload = make_int_vector({1}).value;
  Value unexpected_payload = make_double_vector({2.0}).value;
  inactive_payload.vector.doubles =
      std::move(unexpected_payload.vector.doubles);
  inactive_payload.vector.double_count = 1;
  Value invalid_boolean = make_bool_vector({0, 1}).value;
  invalid_boolean.vector.booleans.get()[1] = 2;
  Value noncanonical_nan = make_double_vector({1.0}).value;
  noncanonical_nan.vector.doubles.get()[0] =
      std::bit_cast<double>(UINT64_C(0xfff8123456789abc));

  const std::array<const Value *, 5> invalid_values{{
      &unknown_container,
      &unknown_scalar,
      &inactive_payload,
      &invalid_boolean,
      &noncanonical_nan,
  }};
  for (const Value *value : invalid_values) {
    CAPTURE(validate_value(*value).invariant);
    CHECK_FALSE(validate_value(*value).ok);

    ScalarType element_type = ScalarType::double_precision;
    const ValueValidationResult element_result =
        value_element_type(*value, element_type);
    CHECK_FALSE(element_result.ok);
    CHECK(element_result.invariant == validate_value(*value).invariant);
    CHECK(element_type == ScalarType::boolean);

    std::size_t rank = 99;
    const ValueValidationResult rank_result = value_rank(*value, rank);
    CHECK_FALSE(rank_result.ok);
    CHECK(rank_result.invariant == validate_value(*value).invariant);
    CHECK(rank == 0);

    std::size_t length = 99;
    const ValueValidationResult length_result = value_length(*value, length);
    CHECK_FALSE(length_result.ok);
    CHECK(length_result.invariant == validate_value(*value).invariant);
    CHECK(length == 0);

    const ScalarProjectionResult projection = project_scalar(*value, 0);
    CHECK_FALSE(projection.ok);
    CHECK(projection.error == ValueAccessError::invalid_value);

    const ValueFormattingResult formatting = format_value(*value);
    CHECK_FALSE(formatting.ok);
    CHECK(formatting.formatted.empty());
    CHECK(formatting.invariant == validate_value(*value).invariant);
    CHECK(formatting.error == ValueFormatError::invalid_value);
  }
}

TEST_CASE("explicit destruction releases owned payload and leaves a valid value") {
  Value value = make_double_vector({1.0, 2.0, 3.0}).value;
  REQUIRE(value.vector.double_count == 3);

  destroy_value(value);

  CHECK(validate_value(value).ok);
  CHECK(value.container == ContainerKind::scalar);
  CHECK(value.scalar.type == ScalarType::boolean);
  CHECK_FALSE(value.scalar.boolean);
  CHECK(value.vector.booleans.get() == nullptr);
  CHECK(value.vector.boolean_count == 0);
  CHECK(value.vector.integers.get() == nullptr);
  CHECK(value.vector.integer_count == 0);
  CHECK(value.vector.doubles.get() == nullptr);
  CHECK(value.vector.double_count == 0);
}

} // namespace bennu
