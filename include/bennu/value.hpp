#ifndef BENNU_VALUE_HPP
#define BENNU_VALUE_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace bennu {

enum class ScalarType {
  boolean,
  integer,
  double_precision,
};

enum class ContainerKind {
  scalar,
  vector,
};

struct ScalarValue {
  ScalarType type;
  bool boolean;
  std::int64_t integer;
  double double_precision;
};

struct VectorValue {
  ScalarType element_type;
  std::unique_ptr<std::uint8_t, decltype(&std::free)> booleans;
  std::size_t boolean_count;
  std::unique_ptr<std::int64_t, decltype(&std::free)> integers;
  std::size_t integer_count;
  std::unique_ptr<double, decltype(&std::free)> doubles;
  std::size_t double_count;
};

struct Value {
  ContainerKind container;
  ScalarValue scalar;
  VectorValue vector;
};

enum class ValueInvariant {
  none,
  unknown_container,
  unknown_scalar_type,
  inactive_scalar_field,
  inactive_vector_payload,
  invalid_boolean_element,
  noncanonical_nan,
};

struct ValueValidationResult {
  bool ok;
  ValueInvariant invariant;
};

enum class ValueAccessError {
  none,
  invalid_value,
  index_out_of_bounds,
};

struct ScalarProjectionResult {
  bool ok;
  ScalarValue value;
  ValueAccessError error;
};

enum class ValueFormatError {
  none,
  invalid_value,
  conversion_failure,
};

struct ValueFormattingResult {
  bool ok;
  std::string formatted;
  ValueInvariant invariant;
  ValueFormatError error;
};

Value make_bool_value(bool value);
Value make_int_value(std::int64_t value);
Value make_double_value(double value);
ValueValidationResult validate_value(const Value &value);
ValueValidationResult value_element_type(const Value &value,
                                         ScalarType &element_type);
ValueValidationResult value_rank(const Value &value, std::size_t &rank);
ValueValidationResult value_length(const Value &value, std::size_t &length);
ScalarProjectionResult project_scalar(const Value &value, std::size_t index);
void destroy_value(Value &value);
ValueFormattingResult format_value(const Value &value);

} // namespace bennu

#endif
