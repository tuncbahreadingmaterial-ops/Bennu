#include "bennu/application.hpp"

#include "typed_application.hpp"

#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace bennu {

namespace {

constexpr std::size_t maximum_application_arity = 2;

Error no_application_error() {
  return make_error(ErrorKind::none, SourceLocation{0, 1, 1});
}

PrimitiveApplicationResult application_failure(Error error) {
  return PrimitiveApplicationResult{false, make_int_value(0), std::move(error)};
}

Value value_from_scalar(const ScalarValue &scalar) {
  switch (scalar.type) {
  case ScalarType::boolean:
    return make_bool_value(scalar.boolean);
  case ScalarType::integer:
    return make_int_value(scalar.integer);
  case ScalarType::double_precision:
    return make_double_value(scalar.double_precision);
  }
  return make_int_value(0);
}

Error primitive_error(ErrorKind kind, const PrimitiveDescriptor &descriptor,
                      SourceLocation location) {
  Error error = make_error(kind, location);
  error.primitive = PrimitiveErrorContext{
      std::string(descriptor.name), std::optional<PrimitiveId>{descriptor.id}};
  return error;
}

Error host_resource_error(PrimitiveApplicationContext &context,
                          const PrimitiveDescriptor &descriptor,
                          HostResourceErrorReason reason,
                          SourceLocation location) {
  Error error = primitive_error(ErrorKind::resource_error, descriptor, location);
  error.resource = ResourceErrorContext{
      reason == HostResourceErrorReason::size_overflow
          ? ResourceErrorReason::size_overflow
          : ResourceErrorReason::allocation_unavailable,
      std::nullopt,
      std::nullopt,
      std::string(execution_profile_name(context.resources.profile)),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
  return error;
}

Error arity_error(const PrimitiveDescriptor &descriptor,
                  std::size_t supplied_arity, SourceLocation location) {
  Error error = primitive_error(ErrorKind::arity_error, descriptor, location);
  ArityErrorContext context{supplied_arity, {}};
  for (std::size_t index = 0; index < descriptor.signature_count; ++index) {
    const std::size_t arity = descriptor.signatures[index].parameter_count;
    if (std::find(context.accepted.begin(), context.accepted.end(), arity) ==
        context.accepted.end()) {
      context.accepted.push_back(arity);
    }
  }
  error.arity = std::move(context);
  return error;
}

bool type_accepts(const PrimitiveDescriptor &descriptor,
                  const PrimitiveSignature &signature,
                  std::size_t argument_index, const Value &argument,
                  ScalarType actual_type) {
  const ValueType parameter = signature.parameters[argument_index];
  if (argument.container == ContainerKind::tuple) {
    return false;
  }
  if (descriptor.lifting == LiftingMode::none) {
    return parameter.container == argument.container &&
           parameter.element == actual_type;
  }
  return actual_type == parameter.element ||
         (actual_type == ScalarType::integer &&
          parameter.element == ScalarType::double_precision);
}

Error type_error(PrimitiveApplicationContext &application_context,
                 const PrimitiveDescriptor &descriptor,
                 std::span<const Value> arguments,
                 std::span<const ScalarType> actual_types,
                 SourceLocation location) {
  Error error = primitive_error(ErrorKind::type_mismatch, descriptor, location);
  TypeErrorContext context;
  context.actual_arguments.reserve(arguments.size());
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    ValueTypeResult actual = value_type(arguments[index]);
    if (actual.ok) {
      context.actual_arguments.push_back(std::move(actual.type));
    } else if (actual.resource_error != HostResourceErrorReason::none) {
      Error resource_error = host_resource_error(
          application_context, descriptor, actual.resource_error, location);
      resource_error.argument_position = index + 1U;
      return resource_error;
    }
  }

  std::vector<const PrimitiveSignature *> candidates;
  for (std::size_t signature_index = 0;
       signature_index < descriptor.signature_count; ++signature_index) {
    const PrimitiveSignature &signature =
        descriptor.signatures[signature_index];
    if (signature.parameter_count != arguments.size()) {
      continue;
    }
    TypeErrorSignatureContext accepted;
    accepted.parameters.reserve(signature.parameter_count);
    for (std::size_t parameter_index = 0;
         parameter_index < signature.parameter_count; ++parameter_index) {
      accepted.parameters.push_back(
          signature.parameters[parameter_index].container ==
                  ContainerKind::scalar
              ? make_scalar_type(signature.parameters[parameter_index].element)
              : make_vector_type(signature.parameters[parameter_index].element));
    }
    accepted.result =
        signature.result.container == ContainerKind::scalar
            ? make_scalar_type(signature.result.element)
            : make_vector_type(signature.result.element);
    context.accepted_signatures.push_back(std::move(accepted));
    candidates.push_back(&signature);
  }

  for (std::size_t argument_index = 0;
       argument_index < arguments.size() && !candidates.empty();
       ++argument_index) {
    std::vector<const PrimitiveSignature *> remaining;
    for (const PrimitiveSignature *candidate : candidates) {
      if (type_accepts(descriptor, *candidate, argument_index,
                       arguments[argument_index],
                       actual_types[argument_index])) {
        remaining.push_back(candidate);
      }
    }
    if (remaining.empty()) {
      error.argument_position = argument_index + 1;
    }
    candidates = std::move(remaining);
  }
  error.type = std::move(context);
  return error;
}

bool store_scalar(Value &result, std::size_t index,
                  const ScalarValue &scalar) {
  if (result.container != ContainerKind::vector ||
      result.vector.element_type != scalar.type) {
    return false;
  }
  switch (scalar.type) {
  case ScalarType::boolean:
    result.vector.booleans.get()[index] = scalar.boolean ? 1U : 0U;
    return true;
  case ScalarType::integer:
    result.vector.integers.get()[index] = scalar.integer;
    return true;
  case ScalarType::double_precision:
    result.vector.doubles.get()[index] = scalar.double_precision;
    return true;
  }
  return false;
}

ScalarValue project_validated_scalar(const Value &value, std::size_t index) {
  if (value.container == ContainerKind::scalar) {
    return value.scalar;
  }
  switch (value.vector.element_type) {
  case ScalarType::boolean:
    return ScalarValue{ScalarType::boolean,
                       value.vector.booleans.get()[index] != 0U, 0, 0.0};
  case ScalarType::integer:
    return ScalarValue{ScalarType::integer, false,
                       value.vector.integers.get()[index], 0.0};
  case ScalarType::double_precision:
    return ScalarValue{ScalarType::double_precision, false, 0,
                       value.vector.doubles.get()[index]};
  }
  return ScalarValue{ScalarType::integer, false, 0, 0.0};
}

std::size_t validated_vector_length(const Value &value) {
  switch (value.vector.element_type) {
  case ScalarType::boolean:
    return value.vector.boolean_count;
  case ScalarType::integer:
    return value.vector.integer_count;
  case ScalarType::double_precision:
    return value.vector.double_count;
  }
  return 0;
}

} // namespace

namespace {

PrimitiveApplicationResult apply_primitive_impl(
    PrimitiveApplicationContext &context,
    const PrimitiveDescriptor &descriptor,
    const PrimitiveSignature *typed_signature,
    std::span<const Value> arguments, SourceLocation call_location) {
  bool arity_exists = false;
  for (std::size_t index = 0; index < descriptor.signature_count; ++index) {
    if (descriptor.signatures[index].parameter_count == arguments.size()) {
      arity_exists = true;
      break;
    }
  }
  if (!arity_exists) {
    return application_failure(
        arity_error(descriptor, arguments.size(), call_location));
  }
  if (arguments.size() > maximum_application_arity) {
    return application_failure(primitive_error(
        ErrorKind::invalid_primitive_table, descriptor, call_location));
  }

  std::array<ScalarType, maximum_application_arity> actual_types{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const ValueValidationResult validation = validate_value(arguments[index]);
    if (!validation.ok) {
      if (validation.resource_error != HostResourceErrorReason::none) {
        Error error = host_resource_error(context, descriptor,
                                          validation.resource_error,
                                          call_location);
        error.argument_position = index + 1U;
        return application_failure(std::move(error));
      }
      Error error = primitive_error(ErrorKind::invalid_value, descriptor,
                                    call_location);
      error.argument_position = index + 1U;
      error.value = ValueErrorContext{validation.invariant, validation.path,
                                      validation.node_index,
                                      validation.edge_index};
      return application_failure(std::move(error));
    }
    ScalarType element_type = ScalarType::boolean;
    const ValueValidationResult element_type_result =
        value_element_type(arguments[index], element_type);
    if (!element_type_result.ok) {
      if (element_type_result.resource_error !=
          HostResourceErrorReason::none) {
        Error error = host_resource_error(context, descriptor,
                                          element_type_result.resource_error,
                                          call_location);
        error.argument_position = index + 1U;
        return application_failure(std::move(error));
      }
      return application_failure(type_error(
          context, descriptor, arguments,
          std::span<const ScalarType>(actual_types.data(), arguments.size()),
          call_location));
    }
    actual_types[index] = element_type;
  }
  const std::span<const ScalarType> actual_type_span(actual_types.data(),
                                                      arguments.size());

  if (descriptor.lifting == LiftingMode::none) {
    const auto matches_arguments =
        [&arguments, actual_type_span](const PrimitiveSignature &signature) {
      if (signature.parameter_count != arguments.size()) {
        return false;
      }
      for (std::size_t argument_index = 0;
           argument_index < arguments.size(); ++argument_index) {
        if (signature.parameters[argument_index].container !=
                arguments[argument_index].container ||
            signature.parameters[argument_index].element !=
                actual_type_span[argument_index]) {
          return false;
        }
      }
      return true;
    };
    const PrimitiveSignature *selected_structural = nullptr;
    if (typed_signature != nullptr) {
      if (matches_arguments(*typed_signature)) {
        selected_structural = typed_signature;
      }
    } else {
      for (std::size_t signature_index = 0U;
           signature_index < descriptor.signature_count; ++signature_index) {
        const PrimitiveSignature &signature =
            descriptor.signatures[signature_index];
        if (matches_arguments(signature)) {
          selected_structural = &signature;
          break;
        }
      }
    }
    if (selected_structural == nullptr) {
      return application_failure(type_error(
          context, descriptor, arguments, actual_type_span, call_location));
    }
    if (selected_structural->implementation !=
            PrimitiveImplementation::iota_integer ||
        descriptor.id != PrimitiveId::iota || arguments.size() != 1) {
      return application_failure(primitive_error(
          ErrorKind::invalid_primitive_table, descriptor, call_location));
    }

    const std::int64_t bound = arguments[0].scalar.integer;
    const std::uint64_t element_count =
        bound > 0 ? static_cast<std::uint64_t>(bound) : UINT64_C(0);
    const std::size_t work_units =
        element_count > std::numeric_limits<std::size_t>::max()
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(element_count);
    VectorAllocationResult allocated = allocate_vector(
        context.resources, ScalarType::integer, element_count, work_units,
        call_location, descriptor.name);
    if (!allocated.ok) {
      if (allocated.error.primitive.has_value()) {
        allocated.error.primitive->id = descriptor.id;
      }
      return application_failure(std::move(allocated.error));
    }
    for (std::size_t index = 0; index < work_units; ++index) {
      allocated.value.vector.integers.get()[index] =
          static_cast<std::int64_t>(index) + std::int64_t{1};
    }
    return PrimitiveApplicationResult{true, std::move(allocated.value),
                                      no_application_error()};
  }

  const PrimitiveSignature *selected_signature = typed_signature;
  if (selected_signature == nullptr) {
    const SignatureSelectionResult selected =
        select_primitive_signature(descriptor, actual_type_span);
    if (selected.status == SignatureSelectionStatus::arity_mismatch) {
      return application_failure(
          arity_error(descriptor, arguments.size(), call_location));
    }
    if (selected.status != SignatureSelectionStatus::success ||
        selected.signature == nullptr) {
      return application_failure(type_error(
          context, descriptor, arguments, actual_type_span, call_location));
    }
    selected_signature = selected.signature;
  } else {
    bool accepts = selected_signature->parameter_count == arguments.size();
    for (std::size_t index = 0U; accepts && index < arguments.size(); ++index) {
      const ScalarType expected = selected_signature->parameters[index].element;
      accepts = actual_type_span[index] == expected ||
                (actual_type_span[index] == ScalarType::integer &&
                 expected == ScalarType::double_precision);
    }
    if (!accepts) {
      return application_failure(type_error(
          context, descriptor, arguments, actual_type_span, call_location));
    }
  }

  bool vector_result = false;
  std::size_t result_length = 1;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index].container != ContainerKind::vector) {
      continue;
    }
    const std::size_t length = validated_vector_length(arguments[index]);
    if (!vector_result) {
      vector_result = true;
      result_length = length;
      continue;
    }
    if (length != result_length) {
      Error error =
          primitive_error(ErrorKind::shape_mismatch, descriptor, call_location);
      error.argument_position = index + 1;
      error.shape = ShapeErrorContext{{result_length}, {length}};
      return application_failure(std::move(error));
    }
  }

  Value vector_value = make_int_value(0);
  if (vector_result) {
    VectorAllocationResult allocated = allocate_vector(
        context.resources, selected_signature->result.element,
        static_cast<std::uint64_t>(result_length), result_length, call_location,
        descriptor.name);
    if (!allocated.ok) {
      if (allocated.error.primitive.has_value()) {
        allocated.error.primitive->id = descriptor.id;
      }
      return application_failure(std::move(allocated.error));
    }
    vector_value = std::move(allocated.value);
  } else {
    const WorkChargeResult admitted = charge_work(
        context.resources, 1, call_location, descriptor.name);
    if (!admitted.ok) {
      Error error = admitted.error;
      if (error.primitive.has_value()) {
        error.primitive->id = descriptor.id;
      }
      return application_failure(std::move(error));
    }
  }

  std::array<ScalarValue, maximum_application_arity> operands{};
  const std::size_t invocation_count = vector_result ? result_length : 1;
  for (std::size_t result_index = 0; result_index < invocation_count;
       ++result_index) {
    for (std::size_t argument_index = 0; argument_index < arguments.size();
         ++argument_index) {
      const ScalarValue projected =
          project_validated_scalar(arguments[argument_index], result_index);
      const ScalarConversionResult converted = convert_scalar(
          projected,
          selected_signature->parameters[argument_index].element);
      if (converted.status != ScalarConversionStatus::success) {
        if (vector_result) {
          release_vector_reservation(context.resources, vector_value);
        }
        return application_failure(primitive_error(
            ErrorKind::type_mismatch, descriptor, call_location));
      }
      operands[argument_index] = converted.value;
    }

    ++context.scalar_kernel_invocations;
    ScalarKernelResult kernel = invoke_scalar_kernel(
        descriptor, *selected_signature,
        std::span<const ScalarValue>(operands.data(), arguments.size()),
        call_location);
    if (kernel.status != ScalarKernelStatus::success) {
      if (kernel.status == ScalarKernelStatus::domain_error && vector_result) {
        kernel.error.element_index = result_index;
      }
      if (vector_result) {
        release_vector_reservation(context.resources, vector_value);
      }
      return application_failure(std::move(kernel.error));
    }
    if (!vector_result) {
      return PrimitiveApplicationResult{true, value_from_scalar(kernel.value),
                                        no_application_error()};
    }
    if (!store_scalar(vector_value, result_index, kernel.value)) {
      release_vector_reservation(context.resources, vector_value);
      return application_failure(primitive_error(
          ErrorKind::invalid_primitive_table, descriptor, call_location));
    }
  }

  return PrimitiveApplicationResult{true, std::move(vector_value),
                                    no_application_error()};
}

} // namespace

PrimitiveApplicationResult
apply_primitive(PrimitiveApplicationContext &context,
                const PrimitiveDescriptor &descriptor,
                std::span<const Value> arguments,
                SourceLocation call_location) {
  return apply_primitive_impl(context, descriptor, nullptr, arguments,
                              call_location);
}

PrimitiveApplicationResult apply_typed_primitive(
    PrimitiveApplicationContext &context,
    const PrimitiveDescriptor &descriptor,
    PrimitiveImplementation implementation, std::span<const Value> arguments,
    SourceLocation call_location) {
  const PrimitiveSignature *selected = nullptr;
  for (std::size_t index = 0U; index < descriptor.signature_count; ++index) {
    if (descriptor.signatures[index].implementation == implementation) {
      if (selected != nullptr) {
        return application_failure(primitive_error(
            ErrorKind::invalid_primitive_table, descriptor, call_location));
      }
      selected = &descriptor.signatures[index];
    }
  }
  if (selected == nullptr || implementation == PrimitiveImplementation::none) {
    return application_failure(primitive_error(
        ErrorKind::invalid_primitive_table, descriptor, call_location));
  }
  return apply_primitive_impl(context, descriptor, selected, arguments,
                              call_location);
}

namespace {

Value test_int_vector(std::initializer_list<std::int64_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  VectorAllocationResult result = copy_int_vector(
      resources, std::span<const std::int64_t>(values.begin(), values.size()),
      SourceLocation{0, 1, 1}, "test-input");
  CHECK(result.ok);
  return std::move(result.value);
}

Value test_int_vector_span(std::span<const std::int64_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  VectorAllocationResult result = copy_int_vector(
      resources, values, SourceLocation{0, 1, 1}, "test-input");
  CHECK(result.ok);
  return std::move(result.value);
}

Value test_bool_vector(std::initializer_list<std::uint8_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  VectorAllocationResult result = copy_bool_vector(
      resources, std::span<const std::uint8_t>(values.begin(), values.size()),
      SourceLocation{0, 1, 1}, "test-input");
  CHECK(result.ok);
  return std::move(result.value);
}

[[maybe_unused]] std::string formatted_value(const Value &value) {
  const ValueFormattingResult result = format_value(value);
  CHECK(result.ok);
  return result.formatted;
}

} // namespace

TEST_CASE("shared application preserves scalar kernel behavior") {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext context{resources, 0};
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  const std::array<Value, 2> arguments{{make_int_value(1), make_int_value(2)}};

  const PrimitiveApplicationResult result =
      apply_primitive(context, add, arguments, SourceLocation{4, 2, 3});

  REQUIRE(result.ok);
  CHECK(result.error.kind == ErrorKind::none);
  CHECK(result.value.container == ContainerKind::scalar);
  CHECK(result.value.scalar.type == ScalarType::integer);
  CHECK(result.value.scalar.integer == 3);
  CHECK(context.scalar_kernel_invocations == 1);
  CHECK(resources.live_evaluation_bytes == 0);
  CHECK(resources.work_units == 1);
}

TEST_CASE("typed application executes the lowered implementation without redispatch") {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext context{resources, 0};
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  const std::array<Value, 2> arguments{{make_int_value(1), make_int_value(2)}};

  const PrimitiveApplicationResult result = apply_typed_primitive(
      context, add, PrimitiveImplementation::add_double, arguments,
      SourceLocation{4, 2, 3});

  REQUIRE(result.ok);
  CHECK(result.value.container == ContainerKind::scalar);
  CHECK(result.value.scalar.type == ScalarType::double_precision);
  CHECK(result.value.scalar.double_precision == doctest::Approx(3.0));
  CHECK(context.scalar_kernel_invocations == 1U);
  CHECK(resources.work_units == 1U);
}

TEST_CASE("shared application broadcasts a scalar over a vector") {
  EvaluationResources input_resources =
      make_trusted_local_resources({std::nullopt});
  const std::array<std::int64_t, 3> input{{1, 2, 3}};
  VectorAllocationResult copied = copy_int_vector(
      input_resources, input, SourceLocation{0, 1, 1}, "test-input");
  REQUIRE(copied.ok);

  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext context{resources, 0};
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  std::array<Value, 2> arguments{{make_int_value(10),
                                  std::move(copied.value)}};

  PrimitiveApplicationResult result =
      apply_primitive(context, add, arguments, SourceLocation{5, 2, 4});

  REQUIRE(result.ok);
  CHECK(result.value.container == ContainerKind::vector);
  CHECK(result.value.vector.element_type == ScalarType::integer);
  const ValueFormattingResult formatted = format_value(result.value);
  REQUIRE(formatted.ok);
  CHECK(formatted.formatted == "(11 12 13)");
  CHECK(context.scalar_kernel_invocations == 3);
  CHECK(resources.live_evaluation_bytes == 24);
  CHECK(resources.work_units == 3);
  release_vector_reservation(resources, result.value);
  CHECK(resources.live_evaluation_bytes == 0);
}

TEST_CASE("structural iota produces one through its positive scalar bound") {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext context{resources, 0};
  const PrimitiveDescriptor &iota = *find_primitive(PrimitiveId::iota);
  const std::array<Value, 1> arguments{{make_int_value(4)}};

  PrimitiveApplicationResult result =
      apply_primitive(context, iota, arguments, SourceLocation{9, 3, 2});

  REQUIRE(result.ok);
  CHECK(result.value.container == ContainerKind::vector);
  CHECK(result.value.vector.element_type == ScalarType::integer);
  const ValueFormattingResult formatted = format_value(result.value);
  REQUIRE(formatted.ok);
  CHECK(formatted.formatted == "(1 2 3 4)");
  CHECK(context.scalar_kernel_invocations == 0);
  CHECK(resources.live_evaluation_bytes == 32);
  CHECK(resources.work_units == 4);
  release_vector_reservation(resources, result.value);
}

TEST_CASE("application errors carry deterministic arity type and shape context") {
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  constexpr SourceLocation location{17, 4, 6};

  EvaluationResources arity_resources =
      make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext arity_context{arity_resources, 0};
  const std::array<Value, 1> arity_arguments{{make_bool_value(true)}};
  const PrimitiveApplicationResult arity =
      apply_primitive(arity_context, add, arity_arguments, location);
  CHECK_FALSE(arity.ok);
  CHECK(arity.error.kind == ErrorKind::arity_error);
  REQUIRE(arity.error.arity.has_value());
  if (arity.error.arity.has_value()) {
    CHECK(arity.error.arity->supplied == 1);
    CHECK(arity.error.arity->accepted == std::vector<std::size_t>{2});
  }
  CHECK_FALSE(arity.error.type.has_value());
  CHECK_FALSE(arity.error.shape.has_value());
  CHECK(arity_context.scalar_kernel_invocations == 0);

  EvaluationResources type_resources =
      make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext type_context{type_resources, 0};
  std::array<Value, 2> type_arguments{{test_int_vector({1, 2}),
                                      test_bool_vector({0, 1, 0})}};
  const PrimitiveApplicationResult type =
      apply_primitive(type_context, add, type_arguments, location);
  CHECK_FALSE(type.ok);
  CHECK(type.error.kind == ErrorKind::type_mismatch);
  REQUIRE(type.error.type.has_value());
  if (type.error.type.has_value()) {
    REQUIRE(type.error.type->actual_arguments.size() == 2);
    if (type.error.type->actual_arguments.size() == 2) {
      const TypeArena &left = type.error.type->actual_arguments[0];
      const TypeArena &right = type.error.type->actual_arguments[1];
      REQUIRE(left.nodes.size() == 1U);
      REQUIRE(right.nodes.size() == 1U);
      CHECK(left.nodes[left.root_index].kind == TypeKind::vector);
      CHECK(left.nodes[left.root_index].scalar == ScalarType::integer);
      CHECK(right.nodes[right.root_index].kind == TypeKind::vector);
      CHECK(right.nodes[right.root_index].scalar == ScalarType::boolean);
    }
    CHECK(type.error.type->accepted_signatures.size() == 2);
  }
  REQUIRE(type.error.argument_position.has_value());
  if (type.error.argument_position.has_value()) {
    CHECK(*type.error.argument_position == 2);
  }
  CHECK_FALSE(type.error.shape.has_value());
  CHECK(type_context.scalar_kernel_invocations == 0);

  EvaluationResources shape_resources =
      make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext shape_context{shape_resources, 0};
  std::array<Value, 2> shape_arguments{{test_int_vector({10}),
                                       test_int_vector({1, 2, 3})}};
  const PrimitiveApplicationResult shape =
      apply_primitive(shape_context, add, shape_arguments, location);
  CHECK_FALSE(shape.ok);
  CHECK(shape.error.kind == ErrorKind::shape_mismatch);
  REQUIRE(shape.error.argument_position.has_value());
  if (shape.error.argument_position.has_value()) {
    CHECK(*shape.error.argument_position == 2);
  }
  REQUIRE(shape.error.shape.has_value());
  if (shape.error.shape.has_value()) {
    CHECK(shape.error.shape->expected == std::vector<std::size_t>{1});
    CHECK(shape.error.shape->actual == std::vector<std::size_t>{3});
  }
  CHECK_FALSE(shape.error.resource.has_value());
  CHECK(shape_context.scalar_kernel_invocations == 0);
}

TEST_CASE("elementwise conformance covers vector positions equal vectors and promotion") {
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  constexpr SourceLocation location{2, 1, 3};

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    std::array<Value, 2> arguments{{test_int_vector({1, 2, 3}),
                                    make_int_value(10)}};
    PrimitiveApplicationResult result =
        apply_primitive(context, add, arguments, location);
    REQUIRE(result.ok);
    CHECK(formatted_value(result.value) == "(11 12 13)");
    CHECK(result.value.vector.element_type == ScalarType::integer);
    CHECK(context.scalar_kernel_invocations == 3);
    release_vector_reservation(resources, result.value);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    std::array<Value, 2> arguments{{test_int_vector({1, 2, 3}),
                                    test_int_vector({10, 20, 30})}};
    PrimitiveApplicationResult result =
        apply_primitive(context, add, arguments, location);
    REQUIRE(result.ok);
    CHECK(formatted_value(result.value) == "(11 22 33)");
    CHECK(context.scalar_kernel_invocations == 3);
    release_vector_reservation(resources, result.value);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    std::array<Value, 2> arguments{{test_int_vector({1, 2, 3}),
                                    make_double_value(0.5)}};
    PrimitiveApplicationResult result =
        apply_primitive(context, add, arguments, location);
    REQUIRE(result.ok);
    CHECK(result.value.vector.element_type == ScalarType::double_precision);
    CHECK(formatted_value(result.value) == "(1.5 2.5 3.5)");
    CHECK(context.scalar_kernel_invocations == 3);
    release_vector_reservation(resources, result.value);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 2> arguments{{
        make_int_value(INT64_C(9007199254740993)), make_double_value(0.0)}};
    const PrimitiveApplicationResult result =
        apply_primitive(context, add, arguments, location);
    REQUIRE(result.ok);
    CHECK(result.value.scalar.type == ScalarType::double_precision);
    CHECK(result.value.scalar.double_precision == 9007199254740992.0);
    CHECK(formatted_value(result.value) == "9.007199254740992e15");
    CHECK(context.scalar_kernel_invocations == 1);
  }
}

TEST_CASE("typed empty lifting preserves promotion shape and result element type") {
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  const PrimitiveDescriptor &equals = *find_primitive(PrimitiveId::equals);
  constexpr SourceLocation location{0, 1, 1};

  struct EmptyCase {
    const PrimitiveDescriptor *descriptor;
    Value left;
    Value right;
    ScalarType expected_type;
  };
  std::array<EmptyCase, 4> cases{{
      {&add, test_int_vector({}), make_int_value(10), ScalarType::integer},
      {&add, test_int_vector({}), make_double_value(0.5),
       ScalarType::double_precision},
      {&equals, test_int_vector({}), make_int_value(10), ScalarType::boolean},
      {&add, test_int_vector({}), test_int_vector({}), ScalarType::integer},
  }};

  for (std::size_t index = 0; index < cases.size(); ++index) {
    CAPTURE(index);
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    std::array<Value, 2> arguments{{std::move(cases[index].left),
                                    std::move(cases[index].right)}};
    PrimitiveApplicationResult result = apply_primitive(
        context, *cases[index].descriptor, arguments, location);
    REQUIRE(result.ok);
    CHECK(result.value.container == ContainerKind::vector);
    CHECK(result.value.vector.element_type == cases[index].expected_type);
    CHECK(formatted_value(result.value) == "()");
    CHECK(context.scalar_kernel_invocations == 0);
    CHECK(resources.work_units == 0);
    CHECK(resources.live_evaluation_bytes == 0);
    CHECK(resources.reservation_ordinal == 0);
  }

  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext context{resources, 0};
  std::array<Value, 2> mismatch_arguments{{test_int_vector({}),
                                          test_int_vector({1})}};
  const PrimitiveApplicationResult mismatch =
      apply_primitive(context, add, mismatch_arguments, location);
  CHECK_FALSE(mismatch.ok);
  CHECK(mismatch.error.kind == ErrorKind::shape_mismatch);
  CHECK(context.scalar_kernel_invocations == 0);
}

TEST_CASE("inc equals and not preserve pointwise kernels and result types") {
  constexpr SourceLocation location{0, 1, 1};

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 1> arguments{{test_int_vector({-1, 0, 1})}};
    PrimitiveApplicationResult result = apply_primitive(
        context, *find_primitive(PrimitiveId::inc), arguments, location);
    REQUIRE(result.ok);
    CHECK(formatted_value(result.value) == "(0 1 2)");
    CHECK(context.scalar_kernel_invocations == 3);
    release_vector_reservation(resources, result.value);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 2> arguments{{make_int_value(2),
                                          test_int_vector({1, 2, 3, 2})}};
    PrimitiveApplicationResult result = apply_primitive(
        context, *find_primitive(PrimitiveId::equals), arguments, location);
    REQUIRE(result.ok);
    CHECK(result.value.vector.element_type == ScalarType::boolean);
    CHECK(formatted_value(result.value) == "(false true false true)");
    release_vector_reservation(resources, result.value);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 1> arguments{{test_bool_vector({0, 1})}};
    PrimitiveApplicationResult result = apply_primitive(
        context, *find_primitive(PrimitiveId::logical_not), arguments, location);
    REQUIRE(result.ok);
    CHECK(formatted_value(result.value) == "(true false)");
    release_vector_reservation(resources, result.value);
  }
}

TEST_CASE("resource preflight precedes scalar and vector domain execution") {
  const PrimitiveDescriptor &inc = *find_primitive(PrimitiveId::inc);
  constexpr SourceLocation location{13, 2, 8};

  EvaluationResources vector_resources = make_bounded_resources(
      ResourceLimits{0, std::nullopt, std::nullopt}, {std::nullopt});
  PrimitiveApplicationContext vector_context{vector_resources, 0};
  const std::array<Value, 1> vector_arguments{{test_int_vector({INT64_MAX})}};
  const PrimitiveApplicationResult vector_result =
      apply_primitive(vector_context, inc, vector_arguments, location);
  CHECK_FALSE(vector_result.ok);
  CHECK(vector_result.error.kind == ErrorKind::resource_error);
  REQUIRE(vector_result.error.resource.has_value());
  if (vector_result.error.resource.has_value()) {
    CHECK(vector_result.error.resource->reason ==
          ResourceErrorReason::profile_limit);
    REQUIRE(vector_result.error.resource->limit_kind.has_value());
    if (vector_result.error.resource->limit_kind.has_value()) {
      CHECK(*vector_result.error.resource->limit_kind ==
            ResourceLimitKind::max_vector_bytes);
    }
  }
  REQUIRE(vector_result.error.primitive.has_value());
  if (vector_result.error.primitive.has_value()) {
    CHECK(vector_result.error.primitive->name == "inc");
    REQUIRE(vector_result.error.primitive->id.has_value());
    if (vector_result.error.primitive->id.has_value()) {
      CHECK(*vector_result.error.primitive->id == PrimitiveId::inc);
    }
  }
  CHECK_FALSE(vector_result.error.domain.has_value());
  CHECK(vector_context.scalar_kernel_invocations == 0);
  CHECK(vector_resources.live_evaluation_bytes == 0);
  CHECK(vector_resources.work_units == 0);

  EvaluationResources scalar_resources = make_bounded_resources(
      ResourceLimits{std::nullopt, std::nullopt, 0}, {std::nullopt});
  PrimitiveApplicationContext scalar_context{scalar_resources, 0};
  const std::array<Value, 1> scalar_arguments{{make_int_value(INT64_MAX)}};
  const PrimitiveApplicationResult scalar_result =
      apply_primitive(scalar_context, inc, scalar_arguments, location);
  CHECK_FALSE(scalar_result.ok);
  CHECK(scalar_result.error.kind == ErrorKind::resource_error);
  CHECK_FALSE(scalar_result.error.domain.has_value());
  CHECK(scalar_context.scalar_kernel_invocations == 0);
  CHECK(scalar_resources.work_units == 0);

  EvaluationResources allocation_resources = make_trusted_local_resources({0});
  PrimitiveApplicationContext allocation_context{allocation_resources, 0};
  const std::array<Value, 1> allocation_arguments{{test_int_vector({1, 2})}};
  const PrimitiveApplicationResult allocation_result =
      apply_primitive(allocation_context, inc, allocation_arguments, location);
  CHECK_FALSE(allocation_result.ok);
  CHECK(allocation_result.error.kind == ErrorKind::resource_error);
  REQUIRE(allocation_result.error.resource.has_value());
  if (allocation_result.error.resource.has_value()) {
    CHECK(allocation_result.error.resource->reason ==
          ResourceErrorReason::allocation_unavailable);
  }
  CHECK(allocation_context.scalar_kernel_invocations == 0);
  CHECK(allocation_resources.live_evaluation_bytes == 0);
  CHECK(allocation_resources.work_units == 0);
  CHECK(allocation_resources.reservation_ordinal == 1);
}

TEST_CASE("lifted domain failure reports the lowest index and no partial vector") {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext context{resources, 0};
  const std::array<Value, 1> arguments{{
      test_int_vector({0, INT64_MAX, INT64_MAX})}};

  const PrimitiveApplicationResult result = apply_primitive(
      context, *find_primitive(PrimitiveId::inc), arguments,
      SourceLocation{21, 5, 4});

  CHECK_FALSE(result.ok);
  CHECK(result.error.kind == ErrorKind::domain_error);
  REQUIRE(result.error.element_index.has_value());
  if (result.error.element_index.has_value()) {
    CHECK(*result.error.element_index == 1);
  }
  REQUIRE(result.error.domain.has_value());
  if (result.error.domain.has_value()) {
    CHECK(result.error.domain->reason == DomainErrorReason::integer_overflow);
  }
  CHECK(context.scalar_kernel_invocations == 2);
  CHECK(resources.live_evaluation_bytes == 0);
  CHECK(resources.work_units == 3);
  CHECK(result.value.container == ContainerKind::scalar);
}

TEST_CASE("iota exact structural contract covers typed empties rejected lifting and resources") {
  const PrimitiveDescriptor &iota = *find_primitive(PrimitiveId::iota);
  constexpr SourceLocation location{7, 2, 2};

  for (const std::int64_t bound : {INT64_C(0), INT64_C(-3)}) {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 1> arguments{{make_int_value(bound)}};
    const PrimitiveApplicationResult result =
        apply_primitive(context, iota, arguments, location);
    REQUIRE(result.ok);
    CHECK(result.value.container == ContainerKind::vector);
    CHECK(result.value.vector.element_type == ScalarType::integer);
    CHECK(formatted_value(result.value) == "()");
    CHECK(context.scalar_kernel_invocations == 0);
    CHECK(resources.work_units == 0);
    CHECK(resources.reservation_ordinal == 0);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 1> arguments{{test_int_vector({3})}};
    const PrimitiveApplicationResult result =
        apply_primitive(context, iota, arguments, location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::type_mismatch);
    REQUIRE(result.error.argument_position.has_value());
    if (result.error.argument_position.has_value()) {
      CHECK(*result.error.argument_position == 1);
    }
    CHECK(context.scalar_kernel_invocations == 0);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 1> arguments{{make_double_value(3.0)}};
    const PrimitiveApplicationResult result =
        apply_primitive(context, iota, arguments, location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::type_mismatch);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 0> arguments{};
    const PrimitiveApplicationResult result =
        apply_primitive(context, iota, arguments, location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::arity_error);
    REQUIRE(result.error.arity.has_value());
    if (result.error.arity.has_value()) {
      CHECK(result.error.arity->supplied == 0);
      CHECK(result.error.arity->accepted == std::vector<std::size_t>{1});
    }
  }

  {
    EvaluationResources resources = make_bounded_resources(
        ResourceLimits{8, std::nullopt, std::nullopt}, {std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 1> arguments{{make_int_value(1000001)}};
    const PrimitiveApplicationResult result =
        apply_primitive(context, iota, arguments, location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::resource_error);
    REQUIRE(result.error.resource.has_value());
    if (result.error.resource.has_value()) {
      CHECK(result.error.resource->reason == ResourceErrorReason::profile_limit);
      REQUIRE(result.error.resource->requested_elements.has_value());
      if (result.error.resource->requested_elements.has_value()) {
        CHECK(*result.error.resource->requested_elements == 1000001);
      }
    }
    CHECK(resources.reservation_ordinal == 0);
  }

  {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 1> arguments{{make_int_value(INT64_MAX)}};
    const PrimitiveApplicationResult result =
        apply_primitive(context, iota, arguments, location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::resource_error);
    REQUIRE(result.error.resource.has_value());
    if (result.error.resource.has_value()) {
      CHECK(result.error.resource->reason == ResourceErrorReason::size_overflow);
    }
    CHECK(resources.reservation_ordinal == 0);
  }

  {
    EvaluationResources resources = make_trusted_local_resources({0});
    PrimitiveApplicationContext context{resources, 0};
    const std::array<Value, 1> arguments{{make_int_value(4)}};
    const PrimitiveApplicationResult result =
        apply_primitive(context, iota, arguments, location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::resource_error);
    REQUIRE(result.error.resource.has_value());
    if (result.error.resource.has_value()) {
      CHECK(result.error.resource->reason ==
            ResourceErrorReason::allocation_unavailable);
    }
    CHECK(resources.live_evaluation_bytes == 0);
    CHECK(resources.work_units == 0);
    CHECK(resources.reservation_ordinal == 1);
  }
}

TEST_CASE("small deterministic shapes obey pointwise integer addition") {
  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  for (std::size_t length = 0; length <= 5; ++length) {
    std::vector<std::int64_t> left(length);
    std::vector<std::int64_t> right(length);
    for (std::size_t index = 0; index < length; ++index) {
      left[index] = static_cast<std::int64_t>(index) - INT64_C(2);
      right[index] = static_cast<std::int64_t>(index * 3);
    }

    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    std::array<Value, 2> arguments{{test_int_vector_span(left),
                                    test_int_vector_span(right)}};
    PrimitiveApplicationResult result = apply_primitive(
        context, add, arguments, SourceLocation{0, 1, 1});
    REQUIRE(result.ok);
    CHECK(result.value.container == ContainerKind::vector);
    std::size_t result_length = 99;
    CHECK(value_length(result.value, result_length).ok);
    CHECK(result_length == length);
    CHECK(context.scalar_kernel_invocations == length);
    for (std::size_t index = 0; index < length; ++index) {
      const ScalarProjectionResult projected = project_scalar(result.value, index);
      REQUIRE(projected.ok);
      if (projected.ok) {
        CHECK(projected.value.integer == left[index] + right[index]);
      }
    }
    release_vector_reservation(resources, result.value);
  }
}

} // namespace bennu
