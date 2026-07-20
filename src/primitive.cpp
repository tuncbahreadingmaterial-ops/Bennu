#include "bennu/primitive.hpp"

#include "doctest/doctest.h"

#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace bennu {

namespace {

constexpr ValueType scalar_bool{ContainerKind::scalar, ScalarType::boolean};
constexpr ValueType scalar_int{ContainerKind::scalar, ScalarType::integer};
constexpr ValueType scalar_double{ContainerKind::scalar,
                                  ScalarType::double_precision};
constexpr ValueType vector_int{ContainerKind::vector, ScalarType::integer};

constexpr std::array<ValueType, 1> int_parameter{{scalar_int}};
constexpr std::array<ValueType, 1> double_parameter{{scalar_double}};
constexpr std::array<ValueType, 1> bool_parameter{{scalar_bool}};
constexpr std::array<ValueType, 2> int_parameters{{scalar_int, scalar_int}};
constexpr std::array<ValueType, 2> double_parameters{{scalar_double,
                                                       scalar_double}};
constexpr std::array<ValueType, 2> bool_parameters{{scalar_bool, scalar_bool}};

constexpr std::array<PrimitiveSignature, 2> inc_signatures{{
    {int_parameter.data(), int_parameter.size(), scalar_int,
     PrimitiveImplementation::inc_integer},
    {double_parameter.data(), double_parameter.size(), scalar_double,
     PrimitiveImplementation::inc_double},
}};
constexpr std::array<PrimitiveSignature, 2> add_signatures{{
    {int_parameters.data(), int_parameters.size(), scalar_int,
     PrimitiveImplementation::add_integer},
    {double_parameters.data(), double_parameters.size(), scalar_double,
     PrimitiveImplementation::add_double},
}};
constexpr std::array<PrimitiveSignature, 3> equals_signatures{{
    {bool_parameters.data(), bool_parameters.size(), scalar_bool,
     PrimitiveImplementation::equals_boolean},
    {int_parameters.data(), int_parameters.size(), scalar_bool,
     PrimitiveImplementation::equals_integer},
    {double_parameters.data(), double_parameters.size(), scalar_bool,
     PrimitiveImplementation::equals_double},
}};
constexpr std::array<PrimitiveSignature, 1> not_signatures{{
    {bool_parameter.data(), bool_parameter.size(), scalar_bool,
     PrimitiveImplementation::logical_not_boolean},
}};
constexpr std::array<PrimitiveSignature, 1> iota_signatures{{
    {int_parameter.data(), int_parameter.size(), vector_int,
     PrimitiveImplementation::iota_integer},
}};

constexpr std::array<PrimitiveDescriptor, 5> production_descriptors{{
    {PrimitiveId::inc, "inc", LiftingMode::elementwise, inc_signatures.data(),
     inc_signatures.size()},
    {PrimitiveId::add, "add", LiftingMode::elementwise, add_signatures.data(),
     add_signatures.size()},
    {PrimitiveId::equals, "equals", LiftingMode::elementwise,
     equals_signatures.data(), equals_signatures.size()},
    {PrimitiveId::logical_not, "not", LiftingMode::elementwise,
     not_signatures.data(), not_signatures.size()},
    {PrimitiveId::iota, "iota", LiftingMode::none, iota_signatures.data(),
     iota_signatures.size()},
}};

PrimitiveTableValidationResult valid_table() {
  return PrimitiveTableValidationResult{true,
                                        PrimitiveTableError::none,
                                        0,
                                        0,
                                        0,
                                        0};
}

PrimitiveTableValidationResult invalid_table(
    PrimitiveTableError error, std::size_t descriptor_index,
    std::size_t signature_index, std::size_t conflicting_descriptor_index,
    std::size_t conflicting_signature_index) {
  return PrimitiveTableValidationResult{false,
                                        error,
                                        descriptor_index,
                                        signature_index,
                                        conflicting_descriptor_index,
                                        conflicting_signature_index};
}

bool same_value_type(ValueType left, ValueType right) {
  return left.container == right.container && left.element == right.element;
}

bool same_signature(const PrimitiveSignature &left,
                    const PrimitiveSignature &right) {
  if (left.parameter_count != right.parameter_count ||
      !same_value_type(left.result, right.result)) {
    return false;
  }
  for (std::size_t index = 0; index < left.parameter_count; ++index) {
    if (!same_value_type(left.parameters[index], right.parameters[index])) {
      return false;
    }
  }
  return true;
}

bool implementation_matches(PrimitiveId id,
                            const PrimitiveSignature &signature) {
  const auto scalar_parameters_match =
      [&signature](std::initializer_list<ScalarType> parameters,
                   ScalarType result_type) {
        if (signature.parameter_count != parameters.size() ||
            signature.result.container != ContainerKind::scalar ||
            signature.result.element != result_type) {
          return false;
        }
        for (std::size_t index = 0; index < parameters.size(); ++index) {
          if (signature.parameters[index].container != ContainerKind::scalar ||
              signature.parameters[index].element !=
                  *(parameters.begin() + static_cast<std::ptrdiff_t>(index))) {
            return false;
          }
        }
        return true;
      };

  switch (signature.implementation) {
  case PrimitiveImplementation::none:
    return false;
  case PrimitiveImplementation::inc_integer:
    return id == PrimitiveId::inc &&
           scalar_parameters_match({ScalarType::integer}, ScalarType::integer);
  case PrimitiveImplementation::inc_double:
    return id == PrimitiveId::inc &&
           scalar_parameters_match({ScalarType::double_precision},
                                   ScalarType::double_precision);
  case PrimitiveImplementation::add_integer:
    return id == PrimitiveId::add &&
           scalar_parameters_match(
               {ScalarType::integer, ScalarType::integer}, ScalarType::integer);
  case PrimitiveImplementation::add_double:
    return id == PrimitiveId::add &&
           scalar_parameters_match(
               {ScalarType::double_precision, ScalarType::double_precision},
               ScalarType::double_precision);
  case PrimitiveImplementation::equals_boolean:
    return id == PrimitiveId::equals &&
           scalar_parameters_match(
               {ScalarType::boolean, ScalarType::boolean}, ScalarType::boolean);
  case PrimitiveImplementation::equals_integer:
    return id == PrimitiveId::equals &&
           scalar_parameters_match(
               {ScalarType::integer, ScalarType::integer}, ScalarType::boolean);
  case PrimitiveImplementation::equals_double:
    return id == PrimitiveId::equals &&
           scalar_parameters_match(
               {ScalarType::double_precision, ScalarType::double_precision},
               ScalarType::boolean);
  case PrimitiveImplementation::logical_not_boolean:
    return id == PrimitiveId::logical_not &&
           scalar_parameters_match({ScalarType::boolean}, ScalarType::boolean);
  case PrimitiveImplementation::iota_integer:
    return id == PrimitiveId::iota && signature.parameter_count == 1 &&
           signature.parameters[0].container == ContainerKind::scalar &&
           signature.parameters[0].element == ScalarType::integer &&
           signature.result.container == ContainerKind::vector &&
           signature.result.element == ScalarType::integer;
  }
  return false;
}

struct TypeAssignment {
  ScalarType type;
  std::size_t index;
  const TypeAssignment *previous;
};

ScalarType assigned_type(const TypeAssignment *assignment,
                         std::size_t index) {
  for (const TypeAssignment *node = assignment; node != nullptr;
       node = node->previous) {
    if (node->index == index) {
      return node->type;
    }
  }
  return ScalarType::boolean;
}

bool conversion_cost(ScalarType actual, ScalarType parameter,
                     std::size_t &cost) {
  if (actual == parameter) {
    cost = 0;
    return true;
  }
  if (actual == ScalarType::integer &&
      parameter == ScalarType::double_precision) {
    cost = 1;
    return true;
  }
  cost = 0;
  return false;
}

bool assignment_is_ambiguous(const PrimitiveDescriptor &descriptor,
                             std::size_t arity,
                             const TypeAssignment *assignment) {
  std::size_t minimum_cost = std::numeric_limits<std::size_t>::max();
  std::size_t minimum_count = 0;
  for (std::size_t signature_index = 0;
       signature_index < descriptor.signature_count; ++signature_index) {
    const PrimitiveSignature &signature =
        descriptor.signatures[signature_index];
    if (signature.parameter_count != arity) {
      continue;
    }
    std::size_t total_cost = 0;
    bool accepted = true;
    for (std::size_t parameter_index = 0; parameter_index < arity;
         ++parameter_index) {
      std::size_t parameter_cost = 0;
      if (!conversion_cost(assigned_type(assignment, parameter_index),
                           signature.parameters[parameter_index].element,
                           parameter_cost)) {
        accepted = false;
        break;
      }
      total_cost += parameter_cost;
    }
    if (!accepted) {
      continue;
    }
    if (total_cost < minimum_cost) {
      minimum_cost = total_cost;
      minimum_count = 1;
    } else if (total_cost == minimum_cost) {
      ++minimum_count;
    }
  }
  return minimum_count > 1;
}

bool has_ambiguous_assignment(const PrimitiveDescriptor &descriptor,
                              std::size_t arity, std::size_t index,
                              const TypeAssignment *assignment) {
  if (index == arity) {
    return assignment_is_ambiguous(descriptor, arity, assignment);
  }
  constexpr std::array<ScalarType, 3> scalar_types{{
      ScalarType::boolean,
      ScalarType::integer,
      ScalarType::double_precision,
  }};
  for (const ScalarType type : scalar_types) {
    const TypeAssignment current{type, index, assignment};
    if (has_ambiguous_assignment(descriptor, arity, index + 1, &current)) {
      return true;
    }
  }
  return false;
}

} // namespace

std::span<const PrimitiveDescriptor> production_primitive_descriptors() {
  return production_descriptors;
}

const PrimitiveDescriptor *find_primitive(PrimitiveId id) {
  for (const PrimitiveDescriptor &descriptor : production_descriptors) {
    if (descriptor.id == id) {
      return &descriptor;
    }
  }
  return nullptr;
}

const PrimitiveDescriptor *find_primitive(std::string_view name) {
  for (const PrimitiveDescriptor &descriptor : production_descriptors) {
    if (descriptor.name == name) {
      return &descriptor;
    }
  }
  return nullptr;
}

PrimitiveTableValidationResult
validate_primitive_table(std::span<const PrimitiveDescriptor> descriptors) {
  if (descriptors.empty()) {
    return invalid_table(PrimitiveTableError::empty_table, 0, 0, 0, 0);
  }
  for (std::size_t descriptor_index = 0;
       descriptor_index < descriptors.size(); ++descriptor_index) {
    const PrimitiveDescriptor &descriptor = descriptors[descriptor_index];
    for (std::size_t previous_index = 0; previous_index < descriptor_index;
         ++previous_index) {
      if (descriptors[previous_index].id == descriptor.id) {
        return invalid_table(PrimitiveTableError::duplicate_id,
                             descriptor_index, 0, previous_index, 0);
      }
      if (descriptors[previous_index].name == descriptor.name) {
        return invalid_table(PrimitiveTableError::duplicate_name,
                             descriptor_index, 0, previous_index, 0);
      }
    }
    if (descriptor.signature_count == 0 || descriptor.signatures == nullptr) {
      return invalid_table(PrimitiveTableError::missing_signature,
                           descriptor_index, 0, descriptor_index, 0);
    }
    for (std::size_t signature_index = 0;
         signature_index < descriptor.signature_count; ++signature_index) {
      const PrimitiveSignature &signature =
          descriptor.signatures[signature_index];
      for (std::size_t previous_index = 0; previous_index < signature_index;
           ++previous_index) {
        if (same_signature(descriptor.signatures[previous_index], signature)) {
          return invalid_table(PrimitiveTableError::duplicate_signature,
                               descriptor_index, signature_index,
                               descriptor_index, previous_index);
        }
      }
      if (signature.parameter_count == 0 || signature.parameters == nullptr) {
        return invalid_table(PrimitiveTableError::zero_arity, descriptor_index,
                             signature_index, descriptor_index,
                             signature_index);
      }
      if (descriptor.lifting == LiftingMode::elementwise) {
        if (signature.result.container != ContainerKind::scalar) {
          return invalid_table(
              PrimitiveTableError::elementwise_non_scalar_signature,
              descriptor_index, signature_index, descriptor_index,
              signature_index);
        }
        for (std::size_t parameter_index = 0;
             parameter_index < signature.parameter_count; ++parameter_index) {
          if (signature.parameters[parameter_index].container !=
              ContainerKind::scalar) {
            return invalid_table(
                PrimitiveTableError::elementwise_non_scalar_signature,
                descriptor_index, signature_index, descriptor_index,
                signature_index);
          }
        }
      }
    }
    if (descriptor.lifting == LiftingMode::elementwise) {
      for (std::size_t signature_index = 0;
           signature_index < descriptor.signature_count; ++signature_index) {
        const std::size_t arity =
            descriptor.signatures[signature_index].parameter_count;
        bool already_checked = false;
        for (std::size_t previous_index = 0; previous_index < signature_index;
             ++previous_index) {
          if (descriptor.signatures[previous_index].parameter_count == arity) {
            already_checked = true;
            break;
          }
        }
        if (!already_checked &&
            has_ambiguous_assignment(descriptor, arity, 0, nullptr)) {
          return invalid_table(PrimitiveTableError::equal_cost_ambiguity,
                               descriptor_index, signature_index,
                               descriptor_index, signature_index);
        }
      }
    }
    for (std::size_t signature_index = 0;
         signature_index < descriptor.signature_count; ++signature_index) {
      if (!implementation_matches(descriptor.id,
                                  descriptor.signatures[signature_index])) {
        return invalid_table(PrimitiveTableError::missing_implementation,
                             descriptor_index, signature_index,
                             descriptor_index, signature_index);
      }
    }
  }
  return valid_table();
}

const PrimitiveTableValidationResult &production_primitive_table_validation() {
  static const PrimitiveTableValidationResult validation =
      validate_primitive_table(production_descriptors);
  return validation;
}

SignatureSelectionResult
select_primitive_signature(const PrimitiveDescriptor &descriptor,
                           std::span<const ScalarType> actual_types) {
  bool arity_exists = false;
  const PrimitiveSignature *selected = nullptr;
  std::size_t selected_index = 0;
  std::size_t selected_cost = std::numeric_limits<std::size_t>::max();
  bool ambiguous = false;
  for (std::size_t signature_index = 0;
       signature_index < descriptor.signature_count; ++signature_index) {
    const PrimitiveSignature &signature =
        descriptor.signatures[signature_index];
    if (signature.parameter_count != actual_types.size()) {
      continue;
    }
    arity_exists = true;
    std::size_t total_cost = 0;
    bool accepted = true;
    for (std::size_t parameter_index = 0;
         parameter_index < actual_types.size(); ++parameter_index) {
      std::size_t parameter_cost = 0;
      if (!conversion_cost(actual_types[parameter_index],
                           signature.parameters[parameter_index].element,
                           parameter_cost)) {
        accepted = false;
        break;
      }
      total_cost += parameter_cost;
    }
    if (!accepted) {
      continue;
    }
    if (total_cost < selected_cost) {
      selected = &signature;
      selected_index = signature_index;
      selected_cost = total_cost;
      ambiguous = false;
    } else if (total_cost == selected_cost) {
      ambiguous = true;
    }
  }
  if (!arity_exists) {
    return SignatureSelectionResult{SignatureSelectionStatus::arity_mismatch,
                                    nullptr, 0, 0};
  }
  if (selected == nullptr) {
    return SignatureSelectionResult{SignatureSelectionStatus::type_mismatch,
                                    nullptr, 0, 0};
  }
  if (ambiguous) {
    return SignatureSelectionResult{SignatureSelectionStatus::ambiguous,
                                    nullptr, 0, 0};
  }
  return SignatureSelectionResult{SignatureSelectionStatus::success, selected,
                                  selected_index, selected_cost};
}

namespace {

constexpr std::uint64_t binary64_exponent_mask =
    UINT64_C(0x7ff0000000000000);
constexpr std::uint64_t binary64_fraction_mask =
    UINT64_C(0x000fffffffffffff);
constexpr std::uint64_t canonical_nan_bits = UINT64_C(0x7ff8000000000000);

bool valid_scalar(const ScalarValue &value) {
  switch (value.type) {
  case ScalarType::boolean:
    return value.integer == 0 &&
           std::bit_cast<std::uint64_t>(value.double_precision) == UINT64_C(0);
  case ScalarType::integer:
    return !value.boolean &&
           std::bit_cast<std::uint64_t>(value.double_precision) == UINT64_C(0);
  case ScalarType::double_precision: {
    if (value.boolean || value.integer != 0) {
      return false;
    }
    const std::uint64_t bits =
        std::bit_cast<std::uint64_t>(value.double_precision);
    if ((bits & binary64_exponent_mask) != binary64_exponent_mask ||
        (bits & binary64_fraction_mask) == 0) {
      return true;
    }
    return bits == canonical_nan_bits;
  }
  }
  return false;
}

double int_to_binary64(std::int64_t value) {
  if (value == 0) {
    return 0.0;
  }
  const bool negative = value < 0;
  const std::uint64_t magnitude =
      negative
          ? static_cast<std::uint64_t>(-(value + 1)) + UINT64_C(1)
          : static_cast<std::uint64_t>(value);
  unsigned most_significant =
      static_cast<unsigned>(std::bit_width(magnitude) - 1);
  std::uint64_t significand = 0;
  if (most_significant <= 52U) {
    significand = magnitude << (52U - most_significant);
  } else {
    const unsigned shift = most_significant - 52U;
    significand = magnitude >> shift;
    const std::uint64_t remainder_mask = (UINT64_C(1) << shift) - UINT64_C(1);
    const std::uint64_t remainder = magnitude & remainder_mask;
    const std::uint64_t halfway = UINT64_C(1) << (shift - 1U);
    if (remainder > halfway ||
        (remainder == halfway && (significand & UINT64_C(1)) != 0)) {
      ++significand;
      if (significand == (UINT64_C(1) << 53U)) {
        significand >>= 1U;
        ++most_significant;
      }
    }
  }
  const std::uint64_t sign = negative ? (UINT64_C(1) << 63U) : UINT64_C(0);
  const std::uint64_t exponent =
      static_cast<std::uint64_t>(most_significant + 1023U) << 52U;
  const std::uint64_t fraction = significand & binary64_fraction_mask;
  return std::bit_cast<double>(sign | exponent | fraction);
}

ScalarValue empty_scalar_value() {
  return ScalarValue{ScalarType::boolean, false, 0, 0.0};
}

} // namespace

ScalarConversionResult convert_scalar(const ScalarValue &value,
                                      ScalarType parameter_type) {
  if (!valid_scalar(value)) {
    return ScalarConversionResult{ScalarConversionStatus::invalid_scalar,
                                  empty_scalar_value()};
  }
  if (value.type == parameter_type) {
    return ScalarConversionResult{ScalarConversionStatus::success, value};
  }
  if (value.type == ScalarType::integer &&
      parameter_type == ScalarType::double_precision) {
    return ScalarConversionResult{
        ScalarConversionStatus::success,
        ScalarValue{ScalarType::double_precision, false, 0,
                    int_to_binary64(value.integer)}};
  }
  return ScalarConversionResult{
      ScalarConversionStatus::unsupported_conversion, empty_scalar_value()};
}

namespace {

bool signature_belongs_to_descriptor(const PrimitiveDescriptor &descriptor,
                                     const PrimitiveSignature &signature) {
  for (std::size_t index = 0; index < descriptor.signature_count; ++index) {
    if (&descriptor.signatures[index] == &signature) {
      return true;
    }
  }
  return false;
}

ScalarKernelResult invalid_kernel_invocation() {
  return ScalarKernelResult{
      ScalarKernelStatus::invalid_invocation,
      empty_scalar_value(),
      make_error(ErrorKind::none, SourceLocation{0, 1, 1}),
  };
}

ScalarKernelResult successful_kernel(ScalarValue value) {
  return ScalarKernelResult{
      ScalarKernelStatus::success,
      value,
      make_error(ErrorKind::none, SourceLocation{0, 1, 1}),
  };
}

ScalarKernelResult integer_overflow(
    const PrimitiveDescriptor &descriptor,
    const PrimitiveSignature &signature,
    std::span<const ScalarValue> operands,
    SourceLocation call_location) {
  Error error = make_error(ErrorKind::domain_error, call_location);
  error.primitive = PrimitiveErrorContext{std::string(descriptor.name),
                                          std::optional<PrimitiveId>{
                                              descriptor.id}};
  ScalarSignatureContext signature_context;
  signature_context.parameter_types.reserve(signature.parameter_count);
  for (std::size_t index = 0; index < signature.parameter_count; ++index) {
    signature_context.parameter_types.push_back(
        signature.parameters[index].element);
  }
  signature_context.result_type = signature.result.element;
  error.domain = DomainErrorContext{
      DomainErrorReason::integer_overflow,
      std::move(signature_context),
      std::vector<ScalarValue>(operands.begin(), operands.end()),
  };
  return ScalarKernelResult{ScalarKernelStatus::domain_error,
                            empty_scalar_value(), std::move(error)};
}

bool binary64_is_nan(double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  return (bits & binary64_exponent_mask) == binary64_exponent_mask &&
         (bits & binary64_fraction_mask) != 0;
}

bool binary64_is_infinity(double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  return (bits & ~UINT64_C(0x8000000000000000)) ==
         binary64_exponent_mask;
}

bool strict_binary64_add(double left, double right, double &result) {
  if (binary64_is_nan(left) || binary64_is_nan(right)) {
    result = std::bit_cast<double>(canonical_nan_bits);
    return true;
  }
  if (binary64_is_infinity(left) && binary64_is_infinity(right) &&
      ((std::bit_cast<std::uint64_t>(left) ^
        std::bit_cast<std::uint64_t>(right)) &
       (UINT64_C(1) << 63U)) != 0) {
    result = std::bit_cast<double>(canonical_nan_bits);
    return true;
  }

#if defined(__x86_64__) || defined(_M_X64)
  const unsigned original_control = _mm_getcsr();
  constexpr unsigned exception_flags = 0x003fU;
  constexpr unsigned denormals_are_zero = 0x0040U;
  constexpr unsigned exception_masks = 0x1f80U;
  constexpr unsigned rounding_control = 0x6000U;
  constexpr unsigned flush_to_zero = 0x8000U;
  const unsigned strict_control =
      (original_control | exception_masks) &
      ~(exception_flags | denormals_are_zero | rounding_control |
        flush_to_zero);
  _mm_setcsr(strict_control);
#elif defined(__aarch64__)
  std::uint64_t original_control = 0;
  std::uint64_t original_status = 0;
  __asm__ volatile("mrs %0, fpcr" : "=r"(original_control));
  __asm__ volatile("mrs %0, fpsr" : "=r"(original_status));
  constexpr std::uint64_t exception_enables = UINT64_C(0x00009f00);
  constexpr std::uint64_t rounding_control = UINT64_C(0x00c00000);
  constexpr std::uint64_t flush_and_default_nan = UINT64_C(0x03000000);
  const std::uint64_t strict_control =
      original_control &
      ~(exception_enables | rounding_control | flush_and_default_nan);
  const std::uint64_t clear_status = 0;
  __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(strict_control) : "memory");
  __asm__ volatile("msr fpsr, %0" : : "r"(clear_status) : "memory");
#else
#error "Bennu requires an x86-64 or AArch64 floating-point environment"
#endif
  volatile double strict_left = left;
  volatile double strict_right = right;
  volatile double strict_result = strict_left + strict_right;
  result = strict_result;
#if defined(__x86_64__) || defined(_M_X64)
  _mm_setcsr(original_control);
#elif defined(__aarch64__)
  __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(original_control) : "memory");
  __asm__ volatile("msr fpsr, %0" : : "r"(original_status) : "memory");
#endif
  result = make_double_value(result).scalar.double_precision;
  return true;
}

} // namespace

ScalarKernelResult invoke_scalar_kernel(
    const PrimitiveDescriptor &descriptor,
    const PrimitiveSignature &signature,
    std::span<const ScalarValue> operands,
    SourceLocation call_location) {
  if (descriptor.lifting != LiftingMode::elementwise ||
      !signature_belongs_to_descriptor(descriptor, signature) ||
      !implementation_matches(descriptor.id, signature) ||
      signature.parameter_count != operands.size() ||
      signature.result.container != ContainerKind::scalar) {
    return invalid_kernel_invocation();
  }
  for (std::size_t index = 0; index < operands.size(); ++index) {
    if (!valid_scalar(operands[index]) ||
        operands[index].type != signature.parameters[index].element) {
      return invalid_kernel_invocation();
    }
  }

  switch (signature.implementation) {
  case PrimitiveImplementation::inc_integer:
    if (operands[0].integer == INT64_MAX) {
      return integer_overflow(descriptor, signature, operands, call_location);
    }
    return successful_kernel(
        make_int_value(operands[0].integer + std::int64_t{1}).scalar);
  case PrimitiveImplementation::inc_double: {
    double result = 0.0;
    if (!strict_binary64_add(operands[0].double_precision, 1.0, result)) {
      return invalid_kernel_invocation();
    }
    return successful_kernel(make_double_value(result).scalar);
  }
  case PrimitiveImplementation::add_integer: {
    const std::int64_t left = operands[0].integer;
    const std::int64_t right = operands[1].integer;
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
      return integer_overflow(descriptor, signature, operands, call_location);
    }
    return successful_kernel(make_int_value(left + right).scalar);
  }
  case PrimitiveImplementation::add_double: {
    double result = 0.0;
    if (!strict_binary64_add(operands[0].double_precision,
                             operands[1].double_precision, result)) {
      return invalid_kernel_invocation();
    }
    return successful_kernel(make_double_value(result).scalar);
  }
  case PrimitiveImplementation::equals_boolean:
    return successful_kernel(
        make_bool_value(operands[0].boolean == operands[1].boolean).scalar);
  case PrimitiveImplementation::equals_integer:
    return successful_kernel(
        make_bool_value(operands[0].integer == operands[1].integer).scalar);
  case PrimitiveImplementation::equals_double:
    if (binary64_is_nan(operands[0].double_precision) ||
        binary64_is_nan(operands[1].double_precision)) {
      return successful_kernel(make_bool_value(false).scalar);
    }
    return successful_kernel(
        make_bool_value(operands[0].double_precision ==
                        operands[1].double_precision)
            .scalar);
  case PrimitiveImplementation::logical_not_boolean:
    return successful_kernel(make_bool_value(!operands[0].boolean).scalar);
  case PrimitiveImplementation::none:
  case PrimitiveImplementation::iota_integer:
    return invalid_kernel_invocation();
  }
  return invalid_kernel_invocation();
}

TEST_CASE("production primitive descriptors are static explicit and valid") {
  const std::span<const PrimitiveDescriptor> descriptors =
      production_primitive_descriptors();
  REQUIRE(descriptors.size() == 5);

  constexpr std::array<PrimitiveId, 5> expected_ids{{
      PrimitiveId::inc,
      PrimitiveId::add,
      PrimitiveId::equals,
      PrimitiveId::logical_not,
      PrimitiveId::iota,
  }};
  constexpr std::array<std::string_view, 5> expected_names{{
      "inc", "add", "equals", "not", "iota",
  }};
  constexpr std::array<LiftingMode, 5> expected_lifting{{
      LiftingMode::elementwise,
      LiftingMode::elementwise,
      LiftingMode::elementwise,
      LiftingMode::elementwise,
      LiftingMode::none,
  }};
  constexpr std::array<std::size_t, 5> expected_signature_counts{{2, 2, 3, 1,
                                                                  1}};

  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    CAPTURE(index);
    CHECK(descriptors[index].id == expected_ids[index]);
    CHECK(std::string(descriptors[index].name) ==
          std::string(expected_names[index]));
    CHECK(descriptors[index].lifting == expected_lifting[index]);
    CHECK(descriptors[index].signature_count ==
          expected_signature_counts[index]);
    CHECK(descriptors[index].signatures != nullptr);
    CHECK(find_primitive(expected_ids[index]) == &descriptors[index]);
    CHECK(find_primitive(expected_names[index]) == &descriptors[index]);
  }
  CHECK(find_primitive("missing") == nullptr);
  CHECK(find_primitive(static_cast<PrimitiveId>(99)) == nullptr);

  const PrimitiveTableValidationResult &validation =
      production_primitive_table_validation();
  CHECK(validation.ok);
  CHECK(validation.error == PrimitiveTableError::none);
  CHECK(validate_primitive_table(descriptors).ok);

  const PrimitiveDescriptor &iota = descriptors[4];
  REQUIRE(iota.signature_count == 1);
  CHECK(iota.signatures[0].parameter_count == 1);
  CHECK(iota.signatures[0].parameters[0].container == ContainerKind::scalar);
  CHECK(iota.signatures[0].parameters[0].element == ScalarType::integer);
  CHECK(iota.signatures[0].result.container == ContainerKind::vector);
  CHECK(iota.signatures[0].result.element == ScalarType::integer);
  CHECK(iota.signatures[0].implementation ==
        PrimitiveImplementation::iota_integer);
}

TEST_CASE("primitive descriptor validation rejects every invalid fixture class") {
  const std::array<ValueType, 1> int_parameter{{scalar_int}};
  const std::array<ValueType, 1> double_parameter{{scalar_double}};
  const std::array<ValueType, 1> vector_parameter{{vector_int}};
  const std::array<ValueType, 2> double_int_parameters{{scalar_double,
                                                         scalar_int}};
  const std::array<ValueType, 2> int_double_parameters{{scalar_int,
                                                         scalar_double}};

  const PrimitiveSignature int_signature{
      int_parameter.data(), int_parameter.size(), scalar_int,
      PrimitiveImplementation::inc_integer};
  const PrimitiveSignature double_signature{
      double_parameter.data(), double_parameter.size(), scalar_double,
      PrimitiveImplementation::inc_double};
  const PrimitiveSignature vector_signature{
      vector_parameter.data(), vector_parameter.size(), vector_int,
      PrimitiveImplementation::inc_integer};
  const PrimitiveSignature zero_arity_signature{
      nullptr, 0, scalar_int, PrimitiveImplementation::inc_integer};
  const PrimitiveSignature missing_implementation{
      int_parameter.data(), int_parameter.size(), scalar_int,
      PrimitiveImplementation::none};
  const std::array<PrimitiveSignature, 2> duplicate_signatures{{
      int_signature,
      int_signature,
  }};
  const std::array<PrimitiveSignature, 2> ambiguous_signatures{{
      PrimitiveSignature{double_int_parameters.data(),
                         double_int_parameters.size(), scalar_double,
                         PrimitiveImplementation::add_double},
      PrimitiveSignature{int_double_parameters.data(),
                         int_double_parameters.size(), scalar_double,
                         PrimitiveImplementation::add_double},
  }};
  const std::array<PrimitiveSignature, 1> int_signatures{{int_signature}};
  const std::array<PrimitiveSignature, 1> double_signatures{{
      double_signature,
  }};
  const std::array<PrimitiveSignature, 1> vector_signatures{{
      vector_signature,
  }};
  const std::array<PrimitiveSignature, 1> zero_arity_signatures{{
      zero_arity_signature,
  }};
  const std::array<PrimitiveSignature, 1> missing_implementations{{
      missing_implementation,
  }};

  const auto descriptor = [](PrimitiveId id, std::string_view name,
                             const PrimitiveSignature *signatures,
                             std::size_t signature_count) {
    return PrimitiveDescriptor{id, name, LiftingMode::elementwise, signatures,
                               signature_count};
  };

  const auto check_invalid = [](std::span<const PrimitiveDescriptor> descriptors,
                                PrimitiveTableError expected) {
    const PrimitiveTableValidationResult result =
        validate_primitive_table(descriptors);
    CHECK_FALSE(result.ok);
    CHECK(result.error == expected);
  };

  const std::array<PrimitiveDescriptor, 2> duplicate_ids{{
      descriptor(PrimitiveId::inc, "inc", int_signatures.data(), 1),
      descriptor(PrimitiveId::inc, "other", double_signatures.data(), 1),
  }};
  check_invalid(duplicate_ids, PrimitiveTableError::duplicate_id);

  const std::array<PrimitiveDescriptor, 2> duplicate_names{{
      descriptor(PrimitiveId::inc, "same", int_signatures.data(), 1),
      descriptor(PrimitiveId::add, "same", double_signatures.data(), 1),
  }};
  check_invalid(duplicate_names, PrimitiveTableError::duplicate_name);

  const std::array<PrimitiveDescriptor, 1> missing_signatures{{
      descriptor(PrimitiveId::inc, "inc", nullptr, 0),
  }};
  check_invalid(missing_signatures, PrimitiveTableError::missing_signature);

  const std::array<PrimitiveDescriptor, 1> duplicate_signature_table{{
      descriptor(PrimitiveId::inc, "inc", duplicate_signatures.data(), 2),
  }};
  check_invalid(duplicate_signature_table,
                PrimitiveTableError::duplicate_signature);

  const std::array<PrimitiveDescriptor, 1> zero_arity{{
      descriptor(PrimitiveId::inc, "inc", zero_arity_signatures.data(), 1),
  }};
  check_invalid(zero_arity, PrimitiveTableError::zero_arity);

  const std::array<PrimitiveDescriptor, 1> missing_dispatch{{
      descriptor(PrimitiveId::inc, "inc", missing_implementations.data(), 1),
  }};
  check_invalid(missing_dispatch, PrimitiveTableError::missing_implementation);

  const std::array<PrimitiveDescriptor, 1> vector_elementwise{{
      descriptor(PrimitiveId::inc, "inc", vector_signatures.data(), 1),
  }};
  check_invalid(vector_elementwise,
                PrimitiveTableError::elementwise_non_scalar_signature);

  const std::array<PrimitiveDescriptor, 1> ambiguous{{
      descriptor(PrimitiveId::add, "add", ambiguous_signatures.data(), 2),
  }};
  check_invalid(ambiguous, PrimitiveTableError::equal_cost_ambiguity);

  check_invalid({}, PrimitiveTableError::empty_table);
}

TEST_CASE("overload selection is Cartesian deterministic and exact-first") {
  struct SelectionCase {
    PrimitiveId id;
    std::array<ScalarType, 2> actual;
    std::size_t arity;
    SignatureSelectionStatus status;
    std::size_t signature_index;
    std::size_t cost;
  };
  constexpr std::array<SelectionCase, 24> cases{{
      {PrimitiveId::inc, {ScalarType::boolean, ScalarType::boolean}, 1,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::inc, {ScalarType::integer, ScalarType::boolean}, 1,
       SignatureSelectionStatus::success, 0, 0},
      {PrimitiveId::inc,
       {ScalarType::double_precision, ScalarType::boolean}, 1,
       SignatureSelectionStatus::success, 1, 0},
      {PrimitiveId::add, {ScalarType::boolean, ScalarType::boolean}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::add, {ScalarType::boolean, ScalarType::integer}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::add,
       {ScalarType::boolean, ScalarType::double_precision}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::add, {ScalarType::integer, ScalarType::boolean}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::add, {ScalarType::integer, ScalarType::integer}, 2,
       SignatureSelectionStatus::success, 0, 0},
      {PrimitiveId::add,
       {ScalarType::integer, ScalarType::double_precision}, 2,
       SignatureSelectionStatus::success, 1, 1},
      {PrimitiveId::add,
       {ScalarType::double_precision, ScalarType::boolean}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::add,
       {ScalarType::double_precision, ScalarType::integer}, 2,
       SignatureSelectionStatus::success, 1, 1},
      {PrimitiveId::add,
       {ScalarType::double_precision, ScalarType::double_precision}, 2,
       SignatureSelectionStatus::success, 1, 0},
      {PrimitiveId::equals, {ScalarType::boolean, ScalarType::boolean}, 2,
       SignatureSelectionStatus::success, 0, 0},
      {PrimitiveId::equals, {ScalarType::boolean, ScalarType::integer}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::equals,
       {ScalarType::boolean, ScalarType::double_precision}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::equals, {ScalarType::integer, ScalarType::boolean}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::equals, {ScalarType::integer, ScalarType::integer}, 2,
       SignatureSelectionStatus::success, 1, 0},
      {PrimitiveId::equals,
       {ScalarType::integer, ScalarType::double_precision}, 2,
       SignatureSelectionStatus::success, 2, 1},
      {PrimitiveId::equals,
       {ScalarType::double_precision, ScalarType::boolean}, 2,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::equals,
       {ScalarType::double_precision, ScalarType::integer}, 2,
       SignatureSelectionStatus::success, 2, 1},
      {PrimitiveId::equals,
       {ScalarType::double_precision, ScalarType::double_precision}, 2,
       SignatureSelectionStatus::success, 2, 0},
      {PrimitiveId::logical_not,
       {ScalarType::boolean, ScalarType::boolean}, 1,
       SignatureSelectionStatus::success, 0, 0},
      {PrimitiveId::logical_not,
       {ScalarType::integer, ScalarType::boolean}, 1,
       SignatureSelectionStatus::type_mismatch, 0, 0},
      {PrimitiveId::logical_not,
       {ScalarType::double_precision, ScalarType::boolean}, 1,
       SignatureSelectionStatus::type_mismatch, 0, 0},
  }};

  for (std::size_t index = 0; index < cases.size(); ++index) {
    CAPTURE(index);
    const SelectionCase &selection_case = cases[index];
    const PrimitiveDescriptor *descriptor = find_primitive(selection_case.id);
    REQUIRE(descriptor != nullptr);
    const SignatureSelectionResult selected = select_primitive_signature(
        *descriptor,
        std::span<const ScalarType>(selection_case.actual.data(),
                                    selection_case.arity));
    CHECK(selected.status == selection_case.status);
    if (selected.status == SignatureSelectionStatus::success) {
      CHECK(selected.signature ==
            &descriptor->signatures[selection_case.signature_index]);
      CHECK(selected.signature_index == selection_case.signature_index);
      CHECK(selected.promotion_cost == selection_case.cost);
    } else {
      CHECK(selected.signature == nullptr);
    }
  }

  const PrimitiveDescriptor &inc = *find_primitive(PrimitiveId::inc);
  const std::array<ScalarType, 0> no_types{};
  const std::array<ScalarType, 2> two_types{{ScalarType::integer,
                                             ScalarType::integer}};
  CHECK(select_primitive_signature(inc, no_types).status ==
        SignatureSelectionStatus::arity_mismatch);
  CHECK(select_primitive_signature(inc, two_types).status ==
        SignatureSelectionStatus::arity_mismatch);

  const std::array<ValueType, 2> double_int{{scalar_double, scalar_int}};
  const std::array<ValueType, 2> int_double{{scalar_int, scalar_double}};
  const std::array<PrimitiveSignature, 2> ambiguous_signatures{{
      {double_int.data(), double_int.size(), scalar_double,
       PrimitiveImplementation::add_double},
      {int_double.data(), int_double.size(), scalar_double,
       PrimitiveImplementation::add_double},
  }};
  const PrimitiveDescriptor ambiguous{
      PrimitiveId::add, "ambiguous", LiftingMode::elementwise,
      ambiguous_signatures.data(), ambiguous_signatures.size()};
  CHECK(select_primitive_signature(ambiguous, two_types).status ==
        SignatureSelectionStatus::ambiguous);
}

TEST_CASE("scalar projection conversion covers every type pair and Int64 bits") {
  struct PromotionCase {
    std::int64_t integer;
    std::uint64_t expected_bits;
  };
  constexpr std::array<PromotionCase, 15> promotions{{
      {INT64_MIN, UINT64_C(0xc3e0000000000000)},
      {-9007199254740995LL, UINT64_C(0xc340000000000002)},
      {-9007199254740994LL, UINT64_C(0xc340000000000001)},
      {-9007199254740993LL, UINT64_C(0xc340000000000000)},
      {-9007199254740992LL, UINT64_C(0xc340000000000000)},
      {-9007199254740991LL, UINT64_C(0xc33fffffffffffff)},
      {-1, UINT64_C(0xbff0000000000000)},
      {0, UINT64_C(0x0000000000000000)},
      {1, UINT64_C(0x3ff0000000000000)},
      {9007199254740991LL, UINT64_C(0x433fffffffffffff)},
      {9007199254740992LL, UINT64_C(0x4340000000000000)},
      {9007199254740993LL, UINT64_C(0x4340000000000000)},
      {9007199254740994LL, UINT64_C(0x4340000000000001)},
      {9007199254740995LL, UINT64_C(0x4340000000000002)},
      {INT64_MAX, UINT64_C(0x43e0000000000000)},
  }};

  const int original_rounding = std::fegetround();
  REQUIRE(original_rounding != -1);
  REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
  for (const PromotionCase &promotion : promotions) {
    CAPTURE(promotion.integer);
    const ScalarConversionResult converted = convert_scalar(
        make_int_value(promotion.integer).scalar, ScalarType::double_precision);
    REQUIRE(converted.status == ScalarConversionStatus::success);
    CHECK(converted.value.type == ScalarType::double_precision);
    CHECK(std::bit_cast<std::uint64_t>(converted.value.double_precision) ==
          promotion.expected_bits);
  }
  CHECK(std::fegetround() == FE_DOWNWARD);
  REQUIRE(std::fesetround(original_rounding) == 0);

  constexpr std::array<ScalarType, 3> types{{
      ScalarType::boolean,
      ScalarType::integer,
      ScalarType::double_precision,
  }};
  const std::array<ScalarValue, 3> values{{
      make_bool_value(true).scalar,
      make_int_value(7).scalar,
      make_double_value(-0.0).scalar,
  }};
  for (std::size_t actual = 0; actual < types.size(); ++actual) {
    for (std::size_t parameter = 0; parameter < types.size(); ++parameter) {
      CAPTURE(actual);
      CAPTURE(parameter);
      const ScalarConversionResult converted =
          convert_scalar(values[actual], types[parameter]);
      const bool accepted = actual == parameter ||
                            (types[actual] == ScalarType::integer &&
                             types[parameter] ==
                                 ScalarType::double_precision);
      CHECK((converted.status == ScalarConversionStatus::success) == accepted);
      if (!accepted) {
        CHECK(converted.status ==
              ScalarConversionStatus::unsupported_conversion);
      }
    }
  }

  ScalarValue malformed = make_int_value(1).scalar;
  malformed.boolean = true;
  CHECK(convert_scalar(malformed, ScalarType::integer).status ==
        ScalarConversionStatus::invalid_scalar);
  malformed = make_int_value(1).scalar;
  malformed.type = static_cast<ScalarType>(99);
  CHECK(convert_scalar(malformed, ScalarType::integer).status ==
        ScalarConversionStatus::invalid_scalar);
}

TEST_CASE("integer scalar kernels cover every boundary and structured overflow") {
  struct IncCase {
    std::int64_t input;
    bool success;
    std::int64_t expected;
  };
  constexpr std::array<IncCase, 5> inc_cases{{
      {INT64_MIN, true, INT64_MIN + 1},
      {-1, true, 0},
      {0, true, 1},
      {INT64_MAX - 1, true, INT64_MAX},
      {INT64_MAX, false, 0},
  }};
  const PrimitiveDescriptor &inc = *find_primitive(PrimitiveId::inc);
  const PrimitiveSignature &inc_integer = inc.signatures[0];
  constexpr SourceLocation location{19, 3, 7};
  for (const IncCase &inc_case : inc_cases) {
    CAPTURE(inc_case.input);
    const std::array<ScalarValue, 1> operands{{
        make_int_value(inc_case.input).scalar,
    }};
    const ScalarKernelResult result =
        invoke_scalar_kernel(inc, inc_integer, operands, location);
    CHECK((result.status == ScalarKernelStatus::success) == inc_case.success);
    if (inc_case.success) {
      CHECK(result.value.type == ScalarType::integer);
      CHECK(result.value.integer == inc_case.expected);
      CHECK(result.error.kind == ErrorKind::none);
    } else {
      CHECK(result.status == ScalarKernelStatus::domain_error);
      CHECK(result.error.kind == ErrorKind::domain_error);
      CHECK(result.error.location.offset == location.offset);
      CHECK(result.error.location.line == location.line);
      CHECK(result.error.location.column == location.column);
      REQUIRE(result.error.primitive.has_value());
      CHECK(result.error.primitive->name == "inc");
      REQUIRE(result.error.primitive->id.has_value());
      CHECK(*result.error.primitive->id == PrimitiveId::inc);
      CHECK_FALSE(result.error.element_index.has_value());
      REQUIRE(result.error.domain.has_value());
      CHECK(result.error.domain->reason ==
            DomainErrorReason::integer_overflow);
      CHECK(result.error.domain->signature.parameter_types ==
            std::vector<ScalarType>{ScalarType::integer});
      CHECK(result.error.domain->signature.result_type == ScalarType::integer);
      REQUIRE(result.error.domain->operands.size() == 1);
      CHECK(result.error.domain->operands[0].type == ScalarType::integer);
      CHECK(result.error.domain->operands[0].integer == inc_case.input);
    }
  }

  struct AddCase {
    std::int64_t left;
    std::int64_t right;
    bool success;
    std::int64_t expected;
  };
  constexpr std::array<AddCase, 18> add_cases{{
      {INT64_MIN, -1, false, 0},
      {-1, INT64_MIN, false, 0},
      {INT64_MIN, 0, true, INT64_MIN},
      {0, INT64_MIN, true, INT64_MIN},
      {INT64_MIN, 1, true, INT64_MIN + 1},
      {1, INT64_MIN, true, INT64_MIN + 1},
      {INT64_MIN, INT64_MAX, true, -1},
      {INT64_MAX, INT64_MIN, true, -1},
      {-1, 1, true, 0},
      {1, -1, true, 0},
      {INT64_MAX, -1, true, INT64_MAX - 1},
      {-1, INT64_MAX, true, INT64_MAX - 1},
      {INT64_MAX, 0, true, INT64_MAX},
      {0, INT64_MAX, true, INT64_MAX},
      {INT64_MAX, 1, false, 0},
      {1, INT64_MAX, false, 0},
      {INT64_MAX, INT64_MAX, false, 0},
      {INT64_MIN, INT64_MIN, false, 0},
  }};
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  const PrimitiveSignature &add_integer = add.signatures[0];
  for (const AddCase &add_case : add_cases) {
    CAPTURE(add_case.left);
    CAPTURE(add_case.right);
    const std::array<ScalarValue, 2> operands{{
        make_int_value(add_case.left).scalar,
        make_int_value(add_case.right).scalar,
    }};
    const ScalarKernelResult result =
        invoke_scalar_kernel(add, add_integer, operands, location);
    CHECK((result.status == ScalarKernelStatus::success) == add_case.success);
    if (add_case.success) {
      CHECK(result.value.integer == add_case.expected);
    } else {
      CHECK(result.status == ScalarKernelStatus::domain_error);
      REQUIRE(result.error.domain.has_value());
      CHECK(result.error.domain->reason ==
            DomainErrorReason::integer_overflow);
      CHECK(result.error.domain->signature.parameter_types ==
            std::vector<ScalarType>{ScalarType::integer,
                                    ScalarType::integer});
      CHECK(result.error.domain->signature.result_type == ScalarType::integer);
      REQUIRE(result.error.domain->operands.size() == 2);
      CHECK(result.error.domain->operands[0].integer == add_case.left);
      CHECK(result.error.domain->operands[1].integer == add_case.right);
    }
  }
}

TEST_CASE("Double inc and add kernels match every normative binary64 vector") {
  struct IncCase {
    std::uint64_t input;
    std::uint64_t expected;
  };
  constexpr std::array<IncCase, 8> inc_cases{{
      {UINT64_C(0xbff0000000000000), UINT64_C(0x0000000000000000)},
      {UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000)},
      {UINT64_C(0x0000000000000001), UINT64_C(0x3ff0000000000000)},
      {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff)},
      {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000)},
      {UINT64_C(0xfff0000000000000), UINT64_C(0xfff0000000000000)},
      {UINT64_C(0x7ff8123456789abc), UINT64_C(0x7ff8000000000000)},
      {UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff8000000000000)},
  }};
  const PrimitiveDescriptor &inc = *find_primitive(PrimitiveId::inc);
  for (const IncCase &inc_case : inc_cases) {
    CAPTURE(inc_case.input);
    const std::array<ScalarValue, 1> operands{{
        make_double_value(std::bit_cast<double>(inc_case.input)).scalar,
    }};
    const ScalarKernelResult result =
        invoke_scalar_kernel(inc, inc.signatures[1], operands, {0, 1, 1});
    REQUIRE(result.status == ScalarKernelStatus::success);
    CHECK(std::bit_cast<std::uint64_t>(result.value.double_precision) ==
          inc_case.expected);
  }

  struct AddCase {
    std::uint64_t left;
    std::uint64_t right;
    std::uint64_t expected;
  };
  constexpr std::array<AddCase, 11> add_cases{{
      {UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
       UINT64_C(0x0000000000000000)},
      {UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000),
       UINT64_C(0x8000000000000000)},
      {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ca0000000000000),
       UINT64_C(0x3ff0000000000000)},
      {UINT64_C(0x3ff0000000000000), UINT64_C(0x3cb8000000000000),
       UINT64_C(0x3ff0000000000002)},
      {UINT64_C(0x0000000000000001), UINT64_C(0x0000000000000001),
       UINT64_C(0x0000000000000002)},
      {UINT64_C(0x0010000000000000), UINT64_C(0x800fffffffffffff),
       UINT64_C(0x0000000000000001)},
      {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff),
       UINT64_C(0x7ff0000000000000)},
      {UINT64_C(0xffefffffffffffff), UINT64_C(0xffefffffffffffff),
       UINT64_C(0xfff0000000000000)},
      {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
       UINT64_C(0x7ff8000000000000)},
      {UINT64_C(0x7ff0000000000001), UINT64_C(0x3ff0000000000000),
       UINT64_C(0x7ff8000000000000)},
      {UINT64_C(0xfff8123456789abc), UINT64_C(0x3ff0000000000000),
       UINT64_C(0x7ff8000000000000)},
  }};
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  const int original_rounding = std::fegetround();
  REQUIRE(original_rounding != -1);
  REQUIRE(std::fesetround(FE_UPWARD) == 0);
#if defined(__x86_64__) || defined(_M_X64)
  const unsigned caller_control = _mm_getcsr();
  constexpr unsigned overflow_mask = 0x0400U;
  constexpr unsigned denormals_are_zero = 0x0040U;
  constexpr unsigned flush_to_zero = 0x8000U;
  const unsigned hostile_control =
      (caller_control | denormals_are_zero | flush_to_zero) & ~overflow_mask;
  _mm_setcsr(hostile_control);
#endif
  for (const AddCase &add_case : add_cases) {
    CAPTURE(add_case.left);
    CAPTURE(add_case.right);
    const std::array<ScalarValue, 2> operands{{
        make_double_value(std::bit_cast<double>(add_case.left)).scalar,
        make_double_value(std::bit_cast<double>(add_case.right)).scalar,
    }};
    const ScalarKernelResult result =
        invoke_scalar_kernel(add, add.signatures[1], operands, {0, 1, 1});
    REQUIRE(result.status == ScalarKernelStatus::success);
    CHECK(std::bit_cast<std::uint64_t>(result.value.double_precision) ==
          add_case.expected);
    CHECK(std::fegetround() == FE_UPWARD);
#if defined(__x86_64__) || defined(_M_X64)
    CHECK(_mm_getcsr() == hostile_control);
#endif
  }
#if defined(__x86_64__) || defined(_M_X64)
  _mm_setcsr(caller_control);
#endif
  REQUIRE(std::fesetround(original_rounding) == 0);
}

TEST_CASE("equals and not kernels cover Bool Int and Double domains") {
  const PrimitiveDescriptor &equals = *find_primitive(PrimitiveId::equals);
  for (const bool left : {false, true}) {
    for (const bool right : {false, true}) {
      const std::array<ScalarValue, 2> operands{{
          make_bool_value(left).scalar,
          make_bool_value(right).scalar,
      }};
      const ScalarKernelResult result = invoke_scalar_kernel(
          equals, equals.signatures[0], operands, {0, 1, 1});
      REQUIRE(result.status == ScalarKernelStatus::success);
      CHECK(result.value.type == ScalarType::boolean);
      CHECK(result.value.boolean == (left == right));
    }
  }

  constexpr std::array<std::int64_t, 3> integers{{INT64_MIN, 0, INT64_MAX}};
  for (const std::int64_t left : integers) {
    for (const std::int64_t right : integers) {
      const std::array<ScalarValue, 2> operands{{
          make_int_value(left).scalar,
          make_int_value(right).scalar,
      }};
      const ScalarKernelResult result = invoke_scalar_kernel(
          equals, equals.signatures[1], operands, {0, 1, 1});
      REQUIRE(result.status == ScalarKernelStatus::success);
      CHECK(result.value.boolean == (left == right));
    }
  }

  struct DoubleEqualsCase {
    std::uint64_t left;
    std::uint64_t right;
    bool expected;
  };
  constexpr std::array<DoubleEqualsCase, 9> double_cases{{
      {UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000), true},
      {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000), true},
      {UINT64_C(0xfff0000000000000), UINT64_C(0xfff0000000000000), true},
      {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000), false},
      {UINT64_C(0x0000000000000001), UINT64_C(0x0000000000000001), true},
      {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff), true},
      {UINT64_C(0x7ff8000000000000), UINT64_C(0x7ff8000000000000), false},
      {UINT64_C(0xfff8123456789abc), UINT64_C(0x3ff0000000000000), false},
      {UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff0000000000001), false},
  }};
  for (const DoubleEqualsCase &double_case : double_cases) {
    const std::array<ScalarValue, 2> operands{{
        make_double_value(std::bit_cast<double>(double_case.left)).scalar,
        make_double_value(std::bit_cast<double>(double_case.right)).scalar,
    }};
    const ScalarKernelResult result = invoke_scalar_kernel(
        equals, equals.signatures[2], operands, {0, 1, 1});
    REQUIRE(result.status == ScalarKernelStatus::success);
    CHECK(result.value.boolean == double_case.expected);
  }

  const PrimitiveDescriptor &logical_not =
      *find_primitive(PrimitiveId::logical_not);
  for (const bool operand : {false, true}) {
    const std::array<ScalarValue, 1> operands{{
        make_bool_value(operand).scalar,
    }};
    const ScalarKernelResult result = invoke_scalar_kernel(
        logical_not, logical_not.signatures[0], operands, {0, 1, 1});
    REQUIRE(result.status == ScalarKernelStatus::success);
    CHECK(result.value.boolean == !operand);
  }
}

TEST_CASE("scalar kernel dispatch rejects unselected or structural invocation") {
  const PrimitiveDescriptor &inc = *find_primitive(PrimitiveId::inc);
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  const PrimitiveDescriptor &iota = *find_primitive(PrimitiveId::iota);
  const std::array<ScalarValue, 1> integer{{make_int_value(1).scalar}};
  const std::array<ScalarValue, 2> integers{{make_int_value(1).scalar,
                                             make_int_value(2).scalar}};
  CHECK(invoke_scalar_kernel(iota, iota.signatures[0], integer, {0, 1, 1})
            .status == ScalarKernelStatus::invalid_invocation);
  CHECK(invoke_scalar_kernel(inc, add.signatures[0], integers, {0, 1, 1})
            .status == ScalarKernelStatus::invalid_invocation);
  CHECK(invoke_scalar_kernel(inc, inc.signatures[0], integers, {0, 1, 1})
            .status == ScalarKernelStatus::invalid_invocation);
  CHECK(invoke_scalar_kernel(inc, inc.signatures[1], integer, {0, 1, 1})
            .status == ScalarKernelStatus::invalid_invocation);

  ScalarValue malformed = make_int_value(1).scalar;
  malformed.boolean = true;
  const std::array<ScalarValue, 1> malformed_operands{{malformed}};
  CHECK(invoke_scalar_kernel(inc, inc.signatures[0], malformed_operands,
                             {0, 1, 1})
            .status == ScalarKernelStatus::invalid_invocation);
}

} // namespace bennu
