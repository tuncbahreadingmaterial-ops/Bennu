#ifndef BENNU_ERROR_HPP
#define BENNU_ERROR_HPP

#include "bennu/primitive_id.hpp"
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
  invalid_byte,
  malformed_literal,
  literal_range_error,
  syntax_error,
  unknown_name,
  type_mismatch,
  empty_expression,
  arity_error,
  shape_mismatch,
  invalid_execution_profile,
  resource_error,
  domain_error,
  invalid_primitive_table,
  formatting_error,
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
  std::optional<PrimitiveId> id{};
};

struct ShapeErrorContext {
  std::vector<std::size_t> expected;
  std::vector<std::size_t> actual;
};

struct ArityErrorContext {
  std::size_t supplied;
  std::vector<std::size_t> accepted;
};

struct ErrorValueType {
  ContainerKind container;
  ScalarType element;
};

struct TypeErrorSignatureContext {
  std::vector<ErrorValueType> parameters;
  ErrorValueType result;
};

struct TypeErrorContext {
  std::vector<ErrorValueType> actual_arguments;
  std::vector<TypeErrorSignatureContext> accepted_signatures;
};

struct Error {
  ErrorKind kind;
  SourceLocation location;
  std::string message;
  std::optional<PrimitiveErrorContext> primitive{};
  std::optional<ArityErrorContext> arity{};
  std::optional<TypeErrorContext> type{};
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
