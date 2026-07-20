#ifndef BENNU_VALUE_HPP
#define BENNU_VALUE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
  std::vector<std::uint8_t> booleans;
  std::vector<std::int64_t> integers;
  std::vector<double> doubles;
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

struct ValueConstructionResult {
  bool ok;
  Value value;
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

Value make_bool_value(bool value);
Value make_int_value(std::int64_t value);
Value make_double_value(double value);
ValueConstructionResult make_bool_vector(std::vector<std::uint8_t> values);
ValueConstructionResult make_int_vector(std::vector<std::int64_t> values);
ValueConstructionResult make_double_vector(std::vector<double> values);
ValueValidationResult validate_value(const Value &value);
ScalarType value_element_type(const Value &value);
std::size_t value_rank(const Value &value);
std::size_t value_length(const Value &value);
ScalarProjectionResult project_scalar(const Value &value, std::size_t index);
void destroy_value(Value &value);
std::string format_value(const Value &value);

} // namespace bennu

#endif
