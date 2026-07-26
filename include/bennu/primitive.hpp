#ifndef BENNU_PRIMITIVE_HPP
#define BENNU_PRIMITIVE_HPP

#include "bennu/error.hpp"
#include "bennu/primitive_id.hpp"
#include "bennu/value.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace bennu {

enum class LiftingMode {
  none,
  elementwise,
};

enum class PrimitiveImplementation {
  none = 0,
  inc_integer = 1,
  inc_double = 2,
  add_integer = 3,
  add_double = 4,
  equals_boolean = 5,
  equals_integer = 6,
  equals_double = 7,
  logical_not_boolean = 8,
  iota_integer = 9,
  logical_and_boolean = 10,
  logical_or_boolean = 11,
  not_equals_boolean = 12,
  not_equals_integer = 13,
  not_equals_double = 14,
  odd_integer = 15,
  even_integer = 16,
  is_positive_integer = 17,
  is_positive_double = 18,
  is_negative_integer = 19,
  is_negative_double = 20,
  less_than_integer = 21,
  less_than_double = 22,
  greater_than_integer = 23,
  greater_than_double = 24,
  dec_integer = 25,
  dec_double = 26,
  neg_integer = 27,
  neg_double = 28,
  abs_integer = 29,
  abs_double = 30,
  sub_integer = 31,
  sub_double = 32,
  mul_integer = 33,
  mul_double = 34,
};

struct ValueType {
  ContainerKind container;
  ScalarType element;
};

struct PrimitiveSignature {
  const ValueType *parameters;
  std::size_t parameter_count;
  ValueType result;
  PrimitiveImplementation implementation;
};

struct PrimitiveDescriptor {
  PrimitiveId id;
  std::string_view name;
  LiftingMode lifting;
  const PrimitiveSignature *signatures;
  std::size_t signature_count;
};

enum class PrimitiveTableError {
  none,
  empty_table,
  duplicate_id,
  duplicate_name,
  reserved_name,
  missing_signature,
  duplicate_signature,
  zero_arity,
  missing_implementation,
  elementwise_non_scalar_signature,
  equal_cost_ambiguity,
};

struct PrimitiveTableValidationResult {
  bool ok;
  PrimitiveTableError error;
  std::size_t descriptor_index;
  std::size_t signature_index;
  std::size_t conflicting_descriptor_index;
  std::size_t conflicting_signature_index;
};

enum class SignatureSelectionStatus {
  success,
  arity_mismatch,
  type_mismatch,
  ambiguous,
};

struct SignatureSelectionResult {
  SignatureSelectionStatus status;
  const PrimitiveSignature *signature;
  std::size_t signature_index;
  std::size_t promotion_cost;
};

enum class ScalarConversionStatus {
  success,
  invalid_scalar,
  unsupported_conversion,
};

struct ScalarConversionResult {
  ScalarConversionStatus status;
  ScalarValue value;
};

enum class ScalarKernelStatus {
  success,
  domain_error,
  invalid_invocation,
};

struct ScalarKernelResult {
  ScalarKernelStatus status;
  ScalarValue value;
  Error error;
};

std::span<const PrimitiveDescriptor> production_primitive_descriptors();
const PrimitiveDescriptor *find_primitive(PrimitiveId id);
const PrimitiveDescriptor *find_primitive(std::string_view name);
PrimitiveTableValidationResult
validate_primitive_table(std::span<const PrimitiveDescriptor> descriptors);
const PrimitiveTableValidationResult &production_primitive_table_validation();
SignatureSelectionResult
select_primitive_signature(const PrimitiveDescriptor &descriptor,
                           std::span<const ScalarType> actual_types);
ScalarConversionResult convert_scalar(const ScalarValue &value,
                                      ScalarType parameter_type);
ScalarKernelResult invoke_scalar_kernel(
    const PrimitiveDescriptor &descriptor,
    const PrimitiveSignature &signature,
    std::span<const ScalarValue> operands,
    SourceLocation call_location);

} // namespace bennu

#endif
