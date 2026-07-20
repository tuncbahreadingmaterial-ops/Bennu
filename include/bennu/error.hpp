#ifndef BENNU_ERROR_HPP
#define BENNU_ERROR_HPP

#include "bennu/value.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace bennu {

struct SourceLocation {
  std::size_t offset;
  std::size_t line;
  std::size_t column;
};

enum class ErrorKind {
  none,
  illegal_character,
  malformed_integer,
  integer_out_of_range,
  unknown_name,
  missing_argument,
  expected_whitespace,
  trailing_token,
  integer_overflow,
  type_mismatch,
  allocation_limit_exceeded,
  empty_expression,
  arity_error,
  shape_mismatch,
  invalid_execution_profile,
  resource_error,
  domain_error,
};

enum class ResourceErrorReason {
  size_overflow,
  profile_limit,
  allocation_unavailable,
};

enum class ResourceLimitKind {
  max_vector_bytes,
  max_live_evaluation_bytes,
  max_work_units,
};

struct ResourceErrorContext {
  ResourceErrorReason reason;
  std::optional<std::size_t> requested_elements;
  std::optional<std::size_t> requested_bytes;
  std::string profile;
  std::optional<ResourceLimitKind> limit_kind;
  std::optional<std::size_t> configured_limit;
  std::optional<std::size_t> usage_before;
  std::optional<std::size_t> refused_charge;
};

struct ScalarSignatureContext {
  std::vector<ScalarType> parameter_types;
  ScalarType result_type;
};

enum class DomainErrorReason {
  integer_overflow,
};

struct DomainErrorContext {
  DomainErrorReason reason;
  ScalarSignatureContext signature;
  std::vector<ScalarValue> operands;
};

struct PrimitiveErrorContext {
  std::string name;
};

struct ShapeErrorContext {
  std::vector<std::size_t> expected;
  std::vector<std::size_t> actual;
};

struct Error {
  ErrorKind kind;
  SourceLocation location;
  std::string message;
  std::optional<PrimitiveErrorContext> primitive{};
  // Argument positions are one-based; vector element indices are zero-based.
  std::optional<std::size_t> argument_position{};
  std::optional<ShapeErrorContext> shape{};
  std::optional<std::size_t> element_index{};
  std::optional<ResourceErrorContext> resource{};
  std::optional<DomainErrorContext> domain{};
};

Error make_error(ErrorKind kind, SourceLocation location, std::string message);
Error make_error(ErrorKind kind, SourceLocation location);

} // namespace bennu

#endif
