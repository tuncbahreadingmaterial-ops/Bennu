#include "bennu/application.hpp"

#include "doctest/doctest.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bennu {
namespace {

constexpr SourceLocation matrix_location{30, 16, 1};

struct PrimitiveMatrixRow {
  PrimitiveId id;
  std::string_view name;
  std::size_t arity;
};

constexpr std::array<PrimitiveMatrixRow, 9> elementwise_matrix{{
    {PrimitiveId::inc, "inc", 1},
    {PrimitiveId::add, "add", 2},
    {PrimitiveId::equals, "equals", 2},
    {PrimitiveId::logical_not, "not", 1},
    {PrimitiveId::dec, "dec", 1},
    {PrimitiveId::neg, "neg", 1},
    {PrimitiveId::abs, "abs", 1},
    {PrimitiveId::sub, "sub", 2},
    {PrimitiveId::mul, "mul", 2},
}};

// BENNU-SPEC-0001 section 16 traceability. "yes" means the named S16-NN
// test below iterates every listed primitive; "n/a" means the primitive lacks
// the required arity, numeric promotion, distinct result type, shape pair, or
// domain failure.
//
// Category                                                     inc add equals not
// S16-01 scalar arguments/result                                yes yes yes    yes
// S16-02 every argument vector position                         yes yes yes    yes
// S16-03 equal vectors in multiple positions                    n/a yes yes    n/a
// S16-04 unequal vectors                                        n/a yes yes    n/a
// S16-05 singleton identity/no broadcasting                     yes yes yes    yes
// S16-06 empty vector with scalars                               yes yes yes    yes
// S16-07 equal-typed empty vector/vector                         n/a yes yes    n/a
// S16-08 mixed numeric empties/promotion                         n/a yes yes    n/a
// S16-09 empty/nonempty mismatch                                 n/a yes yes    n/a
// S16-10 exact overload preference                               yes yes yes    yes
// S16-11 Int-to-Double scalar/vector promotion                   n/a yes yes    n/a
// S16-12 signed-64 precision-loss boundary                       n/a yes yes    n/a
// S16-13 rejected type combinations                              yes yes yes    yes
// S16-14 result type differs from every input                    n/a n/a yes    n/a
// S16-15 wrong arity                                             yes yes yes    yes
// S16-16 type before shape                                       n/a yes yes    n/a
// S16-17 shape before domain                                     n/a yes n/a    n/a
// S16-18 resource before domain                                  yes yes n/a    n/a
// S16-19 lowest deterministic domain index                       yes yes n/a    n/a
// S16-20 empty result invokes zero kernels                       yes yes yes    yes
// S16-21 preflight failures invoke zero kernels                  yes yes yes    yes
// S16-22 pointwise/boundary consistency                          yes yes yes    yes

struct ScalarSpec {
  ScalarType type;
  bool boolean;
  std::int64_t integer;
  double double_precision;
};

constexpr ScalarSpec bool_spec(bool value) {
  return ScalarSpec{ScalarType::boolean, value, 0, 0.0};
}

constexpr ScalarSpec int_spec(std::int64_t value) {
  return ScalarSpec{ScalarType::integer, false, value, 0.0};
}

constexpr ScalarSpec double_spec(double value) {
  return ScalarSpec{ScalarType::double_precision, false, 0, value};
}

Value make_scalar(const ScalarSpec &spec) {
  switch (spec.type) {
  case ScalarType::boolean:
    return make_bool_value(spec.boolean);
  case ScalarType::integer:
    return make_int_value(spec.integer);
  case ScalarType::double_precision:
    return make_double_value(spec.double_precision);
  }
  return make_int_value(0);
}

Value make_scalar(const ScalarValue &scalar) {
  return make_scalar(ScalarSpec{scalar.type, scalar.boolean, scalar.integer,
                                scalar.double_precision});
}

Value int_vector(std::initializer_list<std::int64_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  VectorAllocationResult result = copy_int_vector(
      resources, std::span<const std::int64_t>(values.begin(), values.size()),
      matrix_location, "section16-input");
  REQUIRE(result.ok);
  return std::move(result.value);
}

Value int_vector(std::span<const std::int64_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  VectorAllocationResult result = copy_int_vector(
      resources, values, matrix_location, "section16-input");
  REQUIRE(result.ok);
  return std::move(result.value);
}

Value bool_vector(std::initializer_list<std::uint8_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  VectorAllocationResult result = copy_bool_vector(
      resources, std::span<const std::uint8_t>(values.begin(), values.size()),
      matrix_location, "section16-input");
  REQUIRE(result.ok);
  return std::move(result.value);
}

Value bool_vector(std::span<const std::uint8_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  VectorAllocationResult result = copy_bool_vector(
      resources, values, matrix_location, "section16-input");
  REQUIRE(result.ok);
  return std::move(result.value);
}

Value double_vector(std::initializer_list<double> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  VectorAllocationResult result = copy_double_vector(
      resources, std::span<const double>(values.begin(), values.size()),
      matrix_location, "section16-input");
  REQUIRE(result.ok);
  return std::move(result.value);
}

Value numeric_matrix_vector(ScalarType type, bool right) {
  if (type == ScalarType::integer) {
    return right ? int_vector({1, 0, 3}) : int_vector({1, 2, 3});
  }
  return right ? double_vector({1.0, 0.0, 3.0})
               : double_vector({1.0, 2.0, 3.0});
}

std::string formatted(const Value &value) {
  const ValueFormattingResult result = format_value(value);
  REQUIRE(result.ok);
  return result.formatted;
}

bool same_scalar(const ScalarValue &left, const ScalarValue &right) {
  if (left.type != right.type) {
    return false;
  }
  switch (left.type) {
  case ScalarType::boolean:
    return left.boolean == right.boolean;
  case ScalarType::integer:
    return left.integer == right.integer;
  case ScalarType::double_precision:
    return std::bit_cast<std::uint64_t>(left.double_precision) ==
           std::bit_cast<std::uint64_t>(right.double_precision);
  }
  return false;
}

const PrimitiveDescriptor &matrix_descriptor(PrimitiveId id) {
  const PrimitiveDescriptor *descriptor = find_primitive(id);
  REQUIRE(descriptor != nullptr);
  return *descriptor;
}

std::vector<Value> scalar_arguments(PrimitiveId id) {
  std::vector<Value> arguments;
  switch (id) {
  case PrimitiveId::inc:
    arguments.push_back(make_int_value(2));
    break;
  case PrimitiveId::add:
    arguments.push_back(make_int_value(2));
    arguments.push_back(make_int_value(10));
    break;
  case PrimitiveId::dec:
  case PrimitiveId::neg:
    arguments.push_back(make_int_value(2));
    break;
  case PrimitiveId::abs:
    arguments.push_back(make_int_value(-2));
    break;
  case PrimitiveId::sub:
    arguments.push_back(make_int_value(10));
    arguments.push_back(make_int_value(2));
    break;
  case PrimitiveId::mul:
    arguments.push_back(make_int_value(2));
    arguments.push_back(make_int_value(3));
    break;
  case PrimitiveId::equals:
    arguments.push_back(make_int_value(2));
    arguments.push_back(make_int_value(2));
    break;
  case PrimitiveId::logical_not:
    arguments.push_back(make_bool_value(true));
    break;
  case PrimitiveId::iota:
    break;
  }
  return arguments;
}

std::vector<Value> vector_position_arguments(PrimitiveId id,
                                             std::size_t vector_position) {
  std::vector<Value> arguments = scalar_arguments(id);
  switch (id) {
  case PrimitiveId::inc:
  case PrimitiveId::dec:
  case PrimitiveId::neg:
    arguments[0] = int_vector({1, 2});
    break;
  case PrimitiveId::abs:
    arguments[0] = int_vector({-1, -2});
    break;
  case PrimitiveId::add:
  case PrimitiveId::sub:
  case PrimitiveId::mul:
  case PrimitiveId::equals:
    arguments[vector_position] = int_vector({1, 2});
    break;
  case PrimitiveId::logical_not:
    arguments[0] = bool_vector({0, 1});
    break;
  case PrimitiveId::iota:
    break;
  }
  return arguments;
}

std::string vector_position_expected(PrimitiveId id,
                                     std::size_t vector_position) {
  switch (id) {
  case PrimitiveId::inc:
    return "(2 3)";
  case PrimitiveId::add:
    return vector_position == 0 ? "(11 12)" : "(3 4)";
  case PrimitiveId::dec:
    return "(0 1)";
  case PrimitiveId::neg:
    return "(-1 -2)";
  case PrimitiveId::abs:
    return "(1 2)";
  case PrimitiveId::sub:
    return vector_position == 0 ? "(-1 0)" : "(9 8)";
  case PrimitiveId::mul:
    return vector_position == 0 ? "(3 6)" : "(2 4)";
  case PrimitiveId::equals:
    return "(false true)";
  case PrimitiveId::logical_not:
    return "(true false)";
  case PrimitiveId::iota:
    break;
  }
  return "";
}

ScalarType expected_result_type(PrimitiveId id) {
  if (id == PrimitiveId::equals || id == PrimitiveId::logical_not) {
    return ScalarType::boolean;
  }
  return ScalarType::integer;
}

Value empty_argument(PrimitiveId id) {
  if (id == PrimitiveId::logical_not) {
    return bool_vector({});
  }
  return int_vector({});
}

std::vector<Value> rejected_type_arguments(PrimitiveId id) {
  std::vector<Value> arguments;
  switch (id) {
  case PrimitiveId::inc:
  case PrimitiveId::dec:
  case PrimitiveId::neg:
  case PrimitiveId::abs:
    arguments.push_back(make_bool_value(true));
    break;
  case PrimitiveId::add:
  case PrimitiveId::sub:
  case PrimitiveId::mul:
  case PrimitiveId::equals:
    arguments.push_back(make_bool_value(true));
    arguments.push_back(make_int_value(1));
    break;
  case PrimitiveId::logical_not:
    arguments.push_back(make_int_value(1));
    break;
  case PrimitiveId::iota:
    break;
  }
  return arguments;
}

std::vector<Value> too_many_arguments(PrimitiveId id) {
  std::vector<Value> arguments = scalar_arguments(id);
  if (id == PrimitiveId::logical_not) {
    arguments.push_back(make_bool_value(false));
  } else {
    arguments.push_back(make_int_value(0));
  }
  return arguments;
}

void check_empty_success(PrimitiveApplicationResult &result,
                         PrimitiveApplicationContext &context,
                         EvaluationResources &resources,
                         ScalarType expected_type) {
  REQUIRE(result.ok);
  CHECK(result.value.container == ContainerKind::vector);
  CHECK(result.value.vector.element_type == expected_type);
  std::size_t length = 99;
  REQUIRE(value_length(result.value, length).ok);
  CHECK(length == 0);
  CHECK(formatted(result.value) == "()");
  CHECK(context.scalar_kernel_invocations == 0);
  CHECK(resources.work_units == 0);
  CHECK(resources.live_evaluation_bytes == 0);
  CHECK(resources.reservation_ordinal == 0);
}

} // namespace

TEST_SUITE("BENNU-SPEC-0001 section 16 elementwise matrix") {

TEST_CASE("S16-00 matrix enumerates every initial elementwise primitive") {
  const std::span<const PrimitiveDescriptor> descriptors =
      production_primitive_descriptors();
  std::size_t elementwise_count = 0;
  for (const PrimitiveDescriptor &descriptor : descriptors) {
    if (descriptor.lifting == LiftingMode::elementwise) {
      ++elementwise_count;
    }
  }
  REQUIRE(elementwise_count == elementwise_matrix.size());
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    CAPTURE(std::string(row.name));
    const PrimitiveDescriptor &descriptor = matrix_descriptor(row.id);
    CHECK(std::string(descriptor.name) == std::string(row.name));
    CHECK(descriptor.lifting == LiftingMode::elementwise);
    REQUIRE(descriptor.signature_count > 0);
    for (std::size_t index = 0; index < descriptor.signature_count; ++index) {
      CHECK(descriptor.signatures[index].parameter_count == row.arity);
    }
  }
  for (const PrimitiveDescriptor &descriptor : descriptors) {
    if (descriptor.lifting != LiftingMode::elementwise) {
      continue;
    }
    std::size_t matching_rows = 0;
    for (const PrimitiveMatrixRow &row : elementwise_matrix) {
      if (row.id == descriptor.id && row.name == descriptor.name) {
        ++matching_rows;
      }
    }
    CAPTURE(std::string(descriptor.name));
    CHECK(matching_rows == 1);
  }
}

TEST_CASE("S16-01 scalar arguments produce exact scalar values and result types") {
  struct ScalarCase {
    PrimitiveId id;
    std::array<ScalarSpec, 2> arguments;
    std::size_t arity;
    ScalarSpec expected;
  };
  constexpr std::array<ScalarCase, 26> cases{{
      {PrimitiveId::inc, {int_spec(2), int_spec(0)}, 1, int_spec(3)},
      {PrimitiveId::inc, {double_spec(2.5), int_spec(0)}, 1,
       double_spec(3.5)},
      {PrimitiveId::add, {int_spec(2), int_spec(3)}, 2, int_spec(5)},
      {PrimitiveId::add, {int_spec(2), double_spec(0.5)}, 2,
       double_spec(2.5)},
      {PrimitiveId::add, {double_spec(0.5), int_spec(2)}, 2,
       double_spec(2.5)},
      {PrimitiveId::add, {double_spec(1.5), double_spec(2.5)}, 2,
       double_spec(4.0)},
      {PrimitiveId::equals, {bool_spec(true), bool_spec(false)}, 2,
       bool_spec(false)},
      {PrimitiveId::equals, {int_spec(2), int_spec(2)}, 2, bool_spec(true)},
      {PrimitiveId::equals, {int_spec(2), double_spec(2.0)}, 2,
       bool_spec(true)},
      {PrimitiveId::equals, {double_spec(2.0), int_spec(3)}, 2,
       bool_spec(false)},
      {PrimitiveId::equals, {double_spec(2.0), double_spec(2.0)}, 2,
       bool_spec(true)},
      {PrimitiveId::logical_not, {bool_spec(true), bool_spec(false)}, 1,
       bool_spec(false)},
      {PrimitiveId::dec, {int_spec(2), int_spec(0)}, 1, int_spec(1)},
      {PrimitiveId::dec, {double_spec(2.5), int_spec(0)}, 1,
       double_spec(1.5)},
      {PrimitiveId::neg, {int_spec(-2), int_spec(0)}, 1, int_spec(2)},
      {PrimitiveId::neg, {double_spec(-2.5), int_spec(0)}, 1,
       double_spec(2.5)},
      {PrimitiveId::abs, {int_spec(-2), int_spec(0)}, 1, int_spec(2)},
      {PrimitiveId::abs, {double_spec(-2.5), int_spec(0)}, 1,
       double_spec(2.5)},
      {PrimitiveId::sub, {int_spec(5), int_spec(3)}, 2, int_spec(2)},
      {PrimitiveId::sub, {int_spec(5), double_spec(0.5)}, 2,
       double_spec(4.5)},
      {PrimitiveId::sub, {double_spec(5.5), int_spec(2)}, 2,
       double_spec(3.5)},
      {PrimitiveId::sub, {double_spec(5.5), double_spec(2.5)}, 2,
       double_spec(3.0)},
      {PrimitiveId::mul, {int_spec(-2), int_spec(3)}, 2, int_spec(-6)},
      {PrimitiveId::mul, {int_spec(2), double_spec(0.5)}, 2,
       double_spec(1.0)},
      {PrimitiveId::mul, {double_spec(0.5), int_spec(2)}, 2,
       double_spec(1.0)},
      {PrimitiveId::mul, {double_spec(1.5), double_spec(2.0)}, 2,
       double_spec(3.0)},
  }};

  for (std::size_t case_index = 0; case_index < cases.size(); ++case_index) {
    CAPTURE(case_index);
    const ScalarCase &scalar_case = cases[case_index];
    std::vector<Value> arguments;
    for (std::size_t index = 0; index < scalar_case.arity; ++index) {
      arguments.push_back(make_scalar(scalar_case.arguments[index]));
    }
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(scalar_case.id), arguments, matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.container == ContainerKind::scalar);
    CHECK(same_scalar(result.value.scalar,
                      make_scalar(scalar_case.expected).scalar));
    CHECK(context.scalar_kernel_invocations == 1);
    CHECK(resources.work_units == 1);
    CHECK(resources.live_evaluation_bytes == 0);
  }
}

TEST_CASE("S16-02 every argument position accepts a vector and preserves pointwise order") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    for (std::size_t position = 0; position < row.arity; ++position) {
      CAPTURE(std::string(row.name));
      CAPTURE(position);
      std::vector<Value> arguments = vector_position_arguments(row.id, position);
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(row.id), arguments, matrix_location);
      REQUIRE(result.ok);
      CHECK(result.value.container == ContainerKind::vector);
      CHECK(result.value.vector.element_type == expected_result_type(row.id));
      CHECK(formatted(result.value) ==
            vector_position_expected(row.id, position));
      CHECK(context.scalar_kernel_invocations == 2);
      release_vector_reservation(resources, result.value);
    }
  }
}

TEST_CASE("S16-03 dyadic primitives accept equal vectors in both argument positions") {
  struct EqualVectorCase {
    PrimitiveId id;
    ScalarType left_type;
    ScalarType right_type;
    std::string_view expected;
    ScalarType expected_type;
  };
  constexpr std::array<EqualVectorCase, 17> cases{{
      {PrimitiveId::add, ScalarType::integer, ScalarType::integer,
       "(2 2 6)", ScalarType::integer},
      {PrimitiveId::add, ScalarType::integer, ScalarType::double_precision,
       "(2.0 2.0 6.0)", ScalarType::double_precision},
      {PrimitiveId::add, ScalarType::double_precision, ScalarType::integer,
       "(2.0 2.0 6.0)", ScalarType::double_precision},
      {PrimitiveId::add, ScalarType::double_precision,
       ScalarType::double_precision, "(2.0 2.0 6.0)",
       ScalarType::double_precision},
      {PrimitiveId::equals, ScalarType::boolean, ScalarType::boolean,
       "(true false true)", ScalarType::boolean},
      {PrimitiveId::equals, ScalarType::integer, ScalarType::integer,
       "(true false true)", ScalarType::boolean},
      {PrimitiveId::equals, ScalarType::integer, ScalarType::double_precision,
       "(true false true)", ScalarType::boolean},
      {PrimitiveId::equals, ScalarType::double_precision, ScalarType::integer,
       "(true false true)", ScalarType::boolean},
      {PrimitiveId::equals, ScalarType::double_precision,
       ScalarType::double_precision, "(true false true)",
       ScalarType::boolean},
      {PrimitiveId::sub, ScalarType::integer, ScalarType::integer,
       "(0 2 0)", ScalarType::integer},
      {PrimitiveId::sub, ScalarType::integer, ScalarType::double_precision,
       "(0.0 2.0 0.0)", ScalarType::double_precision},
      {PrimitiveId::sub, ScalarType::double_precision, ScalarType::integer,
       "(0.0 2.0 0.0)", ScalarType::double_precision},
      {PrimitiveId::sub, ScalarType::double_precision,
       ScalarType::double_precision, "(0.0 2.0 0.0)",
       ScalarType::double_precision},
      {PrimitiveId::mul, ScalarType::integer, ScalarType::integer,
       "(1 0 9)", ScalarType::integer},
      {PrimitiveId::mul, ScalarType::integer, ScalarType::double_precision,
       "(1.0 0.0 9.0)", ScalarType::double_precision},
      {PrimitiveId::mul, ScalarType::double_precision, ScalarType::integer,
       "(1.0 0.0 9.0)", ScalarType::double_precision},
      {PrimitiveId::mul, ScalarType::double_precision,
       ScalarType::double_precision, "(1.0 0.0 9.0)",
       ScalarType::double_precision},
  }};
  for (const EqualVectorCase &vector_case : cases) {
    CAPTURE(static_cast<int>(vector_case.id));
    CAPTURE(static_cast<int>(vector_case.left_type));
    CAPTURE(static_cast<int>(vector_case.right_type));
    std::vector<Value> arguments;
    if (vector_case.left_type == ScalarType::boolean) {
      arguments.push_back(bool_vector({0, 1, 1}));
      arguments.push_back(bool_vector({0, 0, 1}));
    } else {
      arguments.push_back(
          numeric_matrix_vector(vector_case.left_type, false));
      arguments.push_back(
          numeric_matrix_vector(vector_case.right_type, true));
    }
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(vector_case.id), arguments, matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.container == ContainerKind::vector);
    CHECK(result.value.vector.element_type == vector_case.expected_type);
    CHECK(formatted(result.value) == std::string(vector_case.expected));
    CHECK(context.scalar_kernel_invocations == 3);
    release_vector_reservation(resources, result.value);
  }
}

TEST_CASE("S16-04 dyadic primitives reject unequal vectors before execution") {
  for (const PrimitiveId id : {PrimitiveId::add, PrimitiveId::sub,
                               PrimitiveId::mul, PrimitiveId::equals}) {
    CAPTURE(static_cast<int>(id));
    std::vector<Value> arguments;
    arguments.push_back(int_vector({1, 2}));
    arguments.push_back(int_vector({1, 2, 3}));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(id), arguments, matrix_location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::shape_mismatch);
    CHECK(context.scalar_kernel_invocations == 0);
    CHECK(resources.work_units == 0);
  }
}

TEST_CASE("S16-05 singleton vectors remain vectors and never broadcast") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    CAPTURE(std::string(row.name));
    std::vector<Value> arguments = scalar_arguments(row.id);
    switch (row.id) {
    case PrimitiveId::inc:
      arguments[0] = int_vector({2});
      break;
    case PrimitiveId::add:
      arguments[0] = int_vector({2});
      break;
    case PrimitiveId::dec:
    case PrimitiveId::neg:
      arguments[0] = int_vector({2});
      break;
    case PrimitiveId::abs:
      arguments[0] = int_vector({-2});
      break;
    case PrimitiveId::sub:
      arguments[0] = int_vector({10});
      break;
    case PrimitiveId::mul:
      arguments[0] = int_vector({2});
      break;
    case PrimitiveId::equals:
      arguments[0] = int_vector({2});
      break;
    case PrimitiveId::logical_not:
      arguments[0] = bool_vector({1});
      break;
    case PrimitiveId::iota:
      break;
    }
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(row.id), arguments, matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.container == ContainerKind::vector);
    CHECK(result.value.vector.element_type == expected_result_type(row.id));
    switch (row.id) {
    case PrimitiveId::inc:
      CHECK(formatted(result.value) == "(3)");
      break;
    case PrimitiveId::add:
      CHECK(formatted(result.value) == "(12)");
      break;
    case PrimitiveId::dec:
      CHECK(formatted(result.value) == "(1)");
      break;
    case PrimitiveId::neg:
      CHECK(formatted(result.value) == "(-2)");
      break;
    case PrimitiveId::abs:
      CHECK(formatted(result.value) == "(2)");
      break;
    case PrimitiveId::sub:
      CHECK(formatted(result.value) == "(8)");
      break;
    case PrimitiveId::mul:
      CHECK(formatted(result.value) == "(6)");
      break;
    case PrimitiveId::equals:
      CHECK(formatted(result.value) == "(true)");
      break;
    case PrimitiveId::logical_not:
      CHECK(formatted(result.value) == "(false)");
      break;
    case PrimitiveId::iota:
      break;
    }
    std::size_t length = 0;
    REQUIRE(value_length(result.value, length).ok);
    CHECK(length == 1);
    CHECK(context.scalar_kernel_invocations == 1);
    release_vector_reservation(resources, result.value);
  }

  for (const PrimitiveId id : {PrimitiveId::add, PrimitiveId::sub,
                               PrimitiveId::mul, PrimitiveId::equals}) {
    for (std::size_t singleton_position = 0; singleton_position < 2;
         ++singleton_position) {
      CAPTURE(static_cast<int>(id));
      CAPTURE(singleton_position);
      std::vector<Value> arguments;
      arguments.push_back(singleton_position == 0 ? int_vector({1})
                                                  : int_vector({1, 2}));
      arguments.push_back(singleton_position == 1 ? int_vector({1})
                                                  : int_vector({1, 2}));
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      const PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(id), arguments, matrix_location);
      CHECK_FALSE(result.ok);
      CHECK(result.error.kind == ErrorKind::shape_mismatch);
      CHECK(context.scalar_kernel_invocations == 0);
    }
  }
}

TEST_CASE("S16-06 typed empty vectors with scalars preserve each primitive result type") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    for (std::size_t position = 0; position < row.arity; ++position) {
      CAPTURE(std::string(row.name));
      CAPTURE(position);
      std::vector<Value> arguments = scalar_arguments(row.id);
      arguments[position] = empty_argument(row.id);
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(row.id), arguments, matrix_location);
      check_empty_success(result, context, resources,
                          expected_result_type(row.id));
    }
  }
}

TEST_CASE("S16-07 equal-typed empty vector pairs preserve dyadic result types") {
  struct EmptyPairCase {
    PrimitiveId id;
    ScalarType input_type;
    ScalarType result_type;
  };
  constexpr std::array<EmptyPairCase, 9> cases{{
      {PrimitiveId::add, ScalarType::integer, ScalarType::integer},
      {PrimitiveId::add, ScalarType::double_precision,
       ScalarType::double_precision},
      {PrimitiveId::sub, ScalarType::integer, ScalarType::integer},
      {PrimitiveId::sub, ScalarType::double_precision,
       ScalarType::double_precision},
      {PrimitiveId::mul, ScalarType::integer, ScalarType::integer},
      {PrimitiveId::mul, ScalarType::double_precision,
       ScalarType::double_precision},
      {PrimitiveId::equals, ScalarType::boolean, ScalarType::boolean},
      {PrimitiveId::equals, ScalarType::integer, ScalarType::boolean},
      {PrimitiveId::equals, ScalarType::double_precision, ScalarType::boolean},
  }};
  for (const EmptyPairCase &empty_case : cases) {
    CAPTURE(static_cast<int>(empty_case.id));
    CAPTURE(static_cast<int>(empty_case.input_type));
    std::vector<Value> arguments;
    if (empty_case.input_type == ScalarType::boolean) {
      arguments.push_back(bool_vector({}));
      arguments.push_back(bool_vector({}));
    } else if (empty_case.input_type == ScalarType::integer) {
      arguments.push_back(int_vector({}));
      arguments.push_back(int_vector({}));
    } else {
      arguments.push_back(double_vector({}));
      arguments.push_back(double_vector({}));
    }
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(empty_case.id), arguments, matrix_location);
    check_empty_success(result, context, resources, empty_case.result_type);
  }
}

TEST_CASE("S16-08 mixed numeric typed empties promote for every numeric dyad in both positions") {
  for (const PrimitiveId id : {PrimitiveId::add, PrimitiveId::sub,
                               PrimitiveId::mul, PrimitiveId::equals}) {
    for (std::size_t int_position = 0; int_position < 2; ++int_position) {
      CAPTURE(static_cast<int>(id));
      CAPTURE(int_position);
      std::vector<Value> arguments;
      arguments.push_back(int_position == 0 ? int_vector({})
                                            : double_vector({}));
      arguments.push_back(int_position == 1 ? int_vector({})
                                            : double_vector({}));
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(id), arguments, matrix_location);
      check_empty_success(result, context, resources,
                          id == PrimitiveId::equals
                              ? ScalarType::boolean
                              : ScalarType::double_precision);
    }
  }
}

TEST_CASE("S16-09 empty and nonempty vectors mismatch for each dyadic primitive and order") {
  for (const PrimitiveId id : {PrimitiveId::add, PrimitiveId::sub,
                               PrimitiveId::mul, PrimitiveId::equals}) {
    for (std::size_t empty_position = 0; empty_position < 2; ++empty_position) {
      CAPTURE(static_cast<int>(id));
      CAPTURE(empty_position);
      std::vector<Value> arguments;
      arguments.push_back(empty_position == 0 ? int_vector({})
                                              : int_vector({1}));
      arguments.push_back(empty_position == 1 ? int_vector({})
                                              : int_vector({1}));
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      const PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(id), arguments, matrix_location);
      CHECK_FALSE(result.ok);
      CHECK(result.error.kind == ErrorKind::shape_mismatch);
      CHECK(context.scalar_kernel_invocations == 0);
      CHECK(resources.work_units == 0);
    }
  }
}

TEST_CASE("S16-10 exact overloads win and retain their declared result types") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    CAPTURE(std::string(row.name));
    std::vector<Value> arguments = scalar_arguments(row.id);
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(row.id), arguments, matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.container == ContainerKind::scalar);
    CHECK(result.value.scalar.type == expected_result_type(row.id));
    CHECK(context.scalar_kernel_invocations == 1);
  }

  // These adjacent Int values compare unequal only if the exact Int overload
  // wins; promoting both to binary64 would collapse them to the same value.
  std::vector<Value> precision_neighbors;
  precision_neighbors.push_back(make_int_value(INT64_C(9007199254740993)));
  precision_neighbors.push_back(make_int_value(INT64_C(9007199254740992)));
  EvaluationResources resources =
      make_trusted_local_resources({std::nullopt});
  PrimitiveApplicationContext context{resources, 0};
  const PrimitiveApplicationResult exact_integer_equals = apply_primitive(
      context, matrix_descriptor(PrimitiveId::equals), precision_neighbors,
      matrix_location);
  REQUIRE(exact_integer_equals.ok);
  CHECK(exact_integer_equals.value.scalar.type == ScalarType::boolean);
  CHECK_FALSE(exact_integer_equals.value.scalar.boolean);
}

TEST_CASE("S16-11 Int-to-Double promotion works in scalar and every vector position") {
  for (const PrimitiveId id : {PrimitiveId::add, PrimitiveId::sub,
                               PrimitiveId::mul, PrimitiveId::equals}) {
    for (std::size_t int_position = 0; int_position < 2; ++int_position) {
      CAPTURE(static_cast<int>(id));
      CAPTURE(int_position);
      std::vector<Value> scalar_args;
      scalar_args.push_back(int_position == 0 ? make_int_value(1)
                                              : make_double_value(1.0));
      scalar_args.push_back(int_position == 1 ? make_int_value(1)
                                              : make_double_value(1.0));
      EvaluationResources scalar_resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext scalar_context{scalar_resources, 0};
      const PrimitiveApplicationResult scalar_result = apply_primitive(
          scalar_context, matrix_descriptor(id), scalar_args, matrix_location);
      REQUIRE(scalar_result.ok);
      CHECK(scalar_result.value.scalar.type ==
            (id == PrimitiveId::equals ? ScalarType::boolean
                                       : ScalarType::double_precision));

      std::vector<Value> vector_args;
      vector_args.push_back(int_position == 0 ? int_vector({1, 2})
                                              : make_double_value(1.0));
      vector_args.push_back(int_position == 1 ? int_vector({1, 2})
                                              : make_double_value(1.0));
      EvaluationResources vector_resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext vector_context{vector_resources, 0};
      PrimitiveApplicationResult vector_result = apply_primitive(
          vector_context, matrix_descriptor(id), vector_args, matrix_location);
      REQUIRE(vector_result.ok);
      CHECK(vector_result.value.vector.element_type ==
            (id == PrimitiveId::equals ? ScalarType::boolean
                                       : ScalarType::double_precision));
      std::string expected = "(2.0 3.0)";
      if (id == PrimitiveId::equals) {
        expected = "(true false)";
      } else if (id == PrimitiveId::sub) {
        expected = int_position == 0 ? "(0.0 1.0)" : "(0.0 -1.0)";
      } else if (id == PrimitiveId::mul) {
        expected = "(1.0 2.0)";
      }
      CHECK(formatted(vector_result.value) == expected);
      CHECK(vector_context.scalar_kernel_invocations == 2);
      release_vector_reservation(vector_resources, vector_result.value);
    }
  }
}

TEST_CASE("S16-12 promotion uses the signed-64 precision-loss boundary in scalar and vector positions") {
  constexpr std::int64_t first_rounded_integer = INT64_C(9007199254740993);
  constexpr double rounded_binary64 = 9007199254740992.0;
  for (const PrimitiveId id : {PrimitiveId::add, PrimitiveId::equals}) {
    for (std::size_t int_position = 0; int_position < 2; ++int_position) {
      CAPTURE(static_cast<int>(id));
      CAPTURE(int_position);
      const double other = id == PrimitiveId::add ? 0.0 : rounded_binary64;
      std::vector<Value> scalar_args;
      scalar_args.push_back(int_position == 0 ? make_int_value(first_rounded_integer)
                                              : make_double_value(other));
      scalar_args.push_back(int_position == 1 ? make_int_value(first_rounded_integer)
                                              : make_double_value(other));
      EvaluationResources scalar_resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext scalar_context{scalar_resources, 0};
      const PrimitiveApplicationResult scalar_result = apply_primitive(
          scalar_context, matrix_descriptor(id), scalar_args, matrix_location);
      REQUIRE(scalar_result.ok);
      if (id == PrimitiveId::add) {
        CHECK(scalar_result.value.scalar.type == ScalarType::double_precision);
        CHECK(scalar_result.value.scalar.double_precision == rounded_binary64);
      } else {
        CHECK(scalar_result.value.scalar.type == ScalarType::boolean);
        CHECK(scalar_result.value.scalar.boolean);
      }

      std::vector<Value> vector_args;
      vector_args.push_back(int_position == 0
                                ? int_vector({first_rounded_integer})
                                : make_double_value(other));
      vector_args.push_back(int_position == 1
                                ? int_vector({first_rounded_integer})
                                : make_double_value(other));
      EvaluationResources vector_resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext vector_context{vector_resources, 0};
      PrimitiveApplicationResult vector_result = apply_primitive(
          vector_context, matrix_descriptor(id), vector_args, matrix_location);
      REQUIRE(vector_result.ok);
      CHECK(formatted(vector_result.value) ==
            (id == PrimitiveId::add ? "(9.007199254740992e15)" : "(true)"));
      release_vector_reservation(vector_resources, vector_result.value);
    }
  }
}

TEST_CASE("S16-13 every primitive rejects unsupported element-type combinations") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    CAPTURE(std::string(row.name));
    std::vector<Value> arguments = rejected_type_arguments(row.id);
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(row.id), arguments, matrix_location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::type_mismatch);
    CHECK(context.scalar_kernel_invocations == 0);
    CHECK(resources.work_units == 0);
  }

  for (const PrimitiveId id : {PrimitiveId::add, PrimitiveId::sub,
                               PrimitiveId::mul, PrimitiveId::equals}) {
    std::vector<Value> reversed;
    reversed.push_back(make_int_value(1));
    reversed.push_back(make_bool_value(true));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(id), reversed, matrix_location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::type_mismatch);
    CHECK(context.scalar_kernel_invocations == 0);
  }

  for (const ScalarSpec invalid : {int_spec(0), double_spec(0.0)}) {
    std::vector<Value> arguments;
    arguments.push_back(make_scalar(invalid));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(PrimitiveId::logical_not), arguments,
        matrix_location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::type_mismatch);
    CHECK(context.scalar_kernel_invocations == 0);
  }
}

TEST_CASE("S16-14 equals returns Bool from numeric scalar and vector inputs") {
  {
    std::vector<Value> arguments;
    arguments.push_back(make_int_value(4));
    arguments.push_back(make_int_value(4));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(PrimitiveId::equals), arguments,
        matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.scalar.type == ScalarType::boolean);
    CHECK(result.value.scalar.boolean);
  }
  {
    std::vector<Value> arguments;
    arguments.push_back(int_vector({4, 5}));
    arguments.push_back(int_vector({4, 0}));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(PrimitiveId::equals), arguments,
        matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.vector.element_type == ScalarType::boolean);
    CHECK(formatted(result.value) == "(true false)");
    release_vector_reservation(resources, result.value);
  }
}

TEST_CASE("S16-15 every primitive rejects missing and excess arity without execution") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    CAPTURE(std::string(row.name));
    {
      std::vector<Value> arguments;
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      const PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(row.id), arguments, matrix_location);
      CHECK_FALSE(result.ok);
      CHECK(result.error.kind == ErrorKind::arity_error);
      REQUIRE(result.error.arity.has_value());
      CHECK(result.error.arity->supplied == 0);
      CHECK(result.error.arity->accepted ==
            std::vector<std::size_t>{row.arity});
      CHECK(context.scalar_kernel_invocations == 0);
    }
    {
      std::vector<Value> arguments = too_many_arguments(row.id);
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      const PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(row.id), arguments, matrix_location);
      CHECK_FALSE(result.ok);
      CHECK(result.error.kind == ErrorKind::arity_error);
      REQUIRE(result.error.arity.has_value());
      CHECK(result.error.arity->supplied == row.arity + 1);
      CHECK(context.scalar_kernel_invocations == 0);
    }
  }
}

TEST_CASE("S16-16 type validation precedes shape validation for every dyadic primitive") {
  for (const PrimitiveId id : {PrimitiveId::add, PrimitiveId::sub,
                               PrimitiveId::mul, PrimitiveId::equals}) {
    CAPTURE(static_cast<int>(id));
    std::vector<Value> arguments;
    arguments.push_back(int_vector({1, 2}));
    arguments.push_back(bool_vector({0, 1, 0}));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(id), arguments, matrix_location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::type_mismatch);
    CHECK_FALSE(result.error.shape.has_value());
    CHECK(context.scalar_kernel_invocations == 0);
    CHECK(resources.work_units == 0);
  }
}

TEST_CASE("S16-17 checked dyad shape validation precedes integer domain validation") {
  for (const PrimitiveId id :
       {PrimitiveId::add, PrimitiveId::sub, PrimitiveId::mul}) {
    std::vector<Value> arguments;
    arguments.push_back(int_vector(
        {id == PrimitiveId::add ? INT64_MAX : INT64_MIN}));
    arguments.push_back(id == PrimitiveId::mul ? int_vector({-1, 1})
                                               : int_vector({1, 2}));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(id), arguments, matrix_location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::shape_mismatch);
    CHECK_FALSE(result.error.domain.has_value());
    CHECK(context.scalar_kernel_invocations == 0);
    CHECK(resources.work_units == 0);
  }
}

TEST_CASE("S16-18 resource preflight precedes every checked arithmetic domain check") {
  for (const PrimitiveId id :
       {PrimitiveId::inc, PrimitiveId::dec, PrimitiveId::neg,
        PrimitiveId::abs, PrimitiveId::add, PrimitiveId::sub,
        PrimitiveId::mul}) {
    CAPTURE(static_cast<int>(id));
    std::vector<Value> arguments;
    arguments.push_back(int_vector(
        {id == PrimitiveId::inc || id == PrimitiveId::add ? INT64_MAX
                                                           : INT64_MIN}));
    if (id == PrimitiveId::add || id == PrimitiveId::sub) {
      arguments.push_back(int_vector({1}));
    } else if (id == PrimitiveId::mul) {
      arguments.push_back(int_vector({-1}));
    }
    EvaluationResources resources = make_bounded_resources(
        ResourceLimits{0, std::nullopt, std::nullopt}, {std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(id), arguments, matrix_location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::resource_error);
    CHECK_FALSE(result.error.domain.has_value());
    CHECK(context.scalar_kernel_invocations == 0);
    CHECK(resources.work_units == 0);
    CHECK(resources.live_evaluation_bytes == 0);
  }
}

TEST_CASE("S16-19 checked arithmetic reports the lowest deterministic domain-failure index") {
  for (const PrimitiveId id :
       {PrimitiveId::inc, PrimitiveId::dec, PrimitiveId::neg,
        PrimitiveId::abs, PrimitiveId::add, PrimitiveId::sub,
        PrimitiveId::mul}) {
    CAPTURE(static_cast<int>(id));
    std::vector<Value> arguments;
    if (id == PrimitiveId::inc || id == PrimitiveId::add) {
      arguments.push_back(int_vector({0, INT64_MAX, INT64_MAX}));
    } else if (id == PrimitiveId::mul) {
      arguments.push_back(int_vector({1, INT64_MIN, INT64_MIN}));
    } else {
      arguments.push_back(int_vector({0, INT64_MIN, INT64_MIN}));
    }
    if (id == PrimitiveId::add || id == PrimitiveId::sub) {
      arguments.push_back(int_vector({0, 1, 1}));
    } else if (id == PrimitiveId::mul) {
      arguments.push_back(int_vector({1, -1, -1}));
    }
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(id), arguments, matrix_location);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::domain_error);
    REQUIRE(result.error.element_index.has_value());
    CHECK(*result.error.element_index == 1);
    CHECK(context.scalar_kernel_invocations == 2);
    CHECK(resources.live_evaluation_bytes == 0);
    CHECK(result.value.container == ContainerKind::scalar);
  }
}

TEST_CASE("S16-20 every primitive invokes zero kernels for an empty result") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    CAPTURE(std::string(row.name));
    std::vector<Value> arguments = scalar_arguments(row.id);
    arguments[0] = empty_argument(row.id);
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(row.id), arguments, matrix_location);
    check_empty_success(result, context, resources,
                        expected_result_type(row.id));
  }
}

TEST_CASE("S16-21 every applicable preflight failure invokes zero kernels") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    CAPTURE(std::string(row.name));
    {
      std::vector<Value> arguments;
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      const PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(row.id), arguments, matrix_location);
      CHECK_FALSE(result.ok);
      CHECK(result.error.kind == ErrorKind::arity_error);
      CHECK(context.scalar_kernel_invocations == 0);
    }
    {
      std::vector<Value> arguments = rejected_type_arguments(row.id);
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      const PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(row.id), arguments, matrix_location);
      CHECK_FALSE(result.ok);
      CHECK(result.error.kind == ErrorKind::type_mismatch);
      CHECK(context.scalar_kernel_invocations == 0);
    }
    {
      std::vector<Value> arguments = vector_position_arguments(row.id, 0);
      EvaluationResources resources = make_bounded_resources(
          ResourceLimits{0, std::nullopt, std::nullopt}, {std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      const PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(row.id), arguments, matrix_location);
      CHECK_FALSE(result.ok);
      CHECK(result.error.kind == ErrorKind::resource_error);
      CHECK(context.scalar_kernel_invocations == 0);
    }
    if (row.arity == 2) {
      std::vector<Value> arguments;
      arguments.push_back(int_vector({1}));
      arguments.push_back(int_vector({1, 2}));
      EvaluationResources resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext context{resources, 0};
      const PrimitiveApplicationResult result = apply_primitive(
          context, matrix_descriptor(row.id), arguments, matrix_location);
      CHECK_FALSE(result.ok);
      CHECK(result.error.kind == ErrorKind::shape_mismatch);
      CHECK(context.scalar_kernel_invocations == 0);
    }
  }
}

TEST_CASE("S16-22 every primitive is pointwise-consistent across deterministic small shapes and boundaries") {
  for (const PrimitiveMatrixRow &row : elementwise_matrix) {
    for (std::size_t length = 0; length <= 5; ++length) {
      CAPTURE(std::string(row.name));
      CAPTURE(length);
      std::vector<std::int64_t> left_ints(length);
      std::vector<std::int64_t> right_ints(length);
      std::vector<std::uint8_t> left_bools(length);
      for (std::size_t index = 0; index < length; ++index) {
        left_ints[index] = static_cast<std::int64_t>(index) - INT64_C(2);
        right_ints[index] = static_cast<std::int64_t>(index * 3);
        left_bools[index] = static_cast<std::uint8_t>(index % 2);
      }

      std::vector<Value> vector_args;
      switch (row.id) {
      case PrimitiveId::inc:
      case PrimitiveId::dec:
      case PrimitiveId::neg:
      case PrimitiveId::abs:
        vector_args.push_back(int_vector(left_ints));
        break;
      case PrimitiveId::add:
      case PrimitiveId::sub:
      case PrimitiveId::mul:
      case PrimitiveId::equals:
        vector_args.push_back(int_vector(left_ints));
        vector_args.push_back(int_vector(right_ints));
        break;
      case PrimitiveId::logical_not:
        vector_args.push_back(bool_vector(left_bools));
        break;
      case PrimitiveId::iota:
        break;
      }

      EvaluationResources vector_resources =
          make_trusted_local_resources({std::nullopt});
      PrimitiveApplicationContext vector_context{vector_resources, 0};
      PrimitiveApplicationResult vector_result = apply_primitive(
          vector_context, matrix_descriptor(row.id), vector_args,
          matrix_location);
      REQUIRE(vector_result.ok);
      CHECK(vector_context.scalar_kernel_invocations == length);

      for (std::size_t index = 0; index < length; ++index) {
        std::vector<Value> scalar_args;
        for (const Value &argument : vector_args) {
          const ScalarProjectionResult projected = project_scalar(argument, index);
          REQUIRE(projected.ok);
          scalar_args.push_back(make_scalar(projected.value));
        }
        EvaluationResources scalar_resources =
            make_trusted_local_resources({std::nullopt});
        PrimitiveApplicationContext scalar_context{scalar_resources, 0};
        const PrimitiveApplicationResult scalar_result = apply_primitive(
            scalar_context, matrix_descriptor(row.id), scalar_args,
            matrix_location);
        REQUIRE(scalar_result.ok);
        const ScalarProjectionResult vector_element =
            project_scalar(vector_result.value, index);
        REQUIRE(vector_element.ok);
        CHECK(same_scalar(vector_element.value, scalar_result.value.scalar));
        CHECK(scalar_context.scalar_kernel_invocations == 1);
      }
      release_vector_reservation(vector_resources, vector_result.value);
    }
  }

  for (const std::int64_t boundary : {INT64_MIN, INT64_MAX - INT64_C(1)}) {
    std::vector<Value> arguments;
    arguments.push_back(make_int_value(boundary));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(PrimitiveId::inc), arguments,
        matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.scalar.integer == boundary + INT64_C(1));
  }
  for (const std::array<std::int64_t, 2> operands : {
           std::array<std::int64_t, 2>{INT64_MIN, 0},
           std::array<std::int64_t, 2>{INT64_MAX, 0}}) {
    std::vector<Value> arguments;
    arguments.push_back(make_int_value(operands[0]));
    arguments.push_back(make_int_value(operands[1]));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(PrimitiveId::add), arguments,
        matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.scalar.integer == operands[0]);
  }
  for (const bool value : {false, true}) {
    std::vector<Value> arguments;
    arguments.push_back(make_bool_value(value));
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext context{resources, 0};
    const PrimitiveApplicationResult result = apply_primitive(
        context, matrix_descriptor(PrimitiveId::logical_not), arguments,
        matrix_location);
    REQUIRE(result.ok);
    CHECK(result.value.scalar.boolean == !value);
  }

  struct DomainBoundaryCase {
    PrimitiveId id;
    std::array<std::int64_t, 2> operands;
    std::size_t arity;
  };
  constexpr std::array<DomainBoundaryCase, 3> domain_cases{{
      {PrimitiveId::inc, {INT64_MAX, 0}, 1},
      {PrimitiveId::add, {INT64_MAX, 1}, 2},
      {PrimitiveId::add, {INT64_MIN, -1}, 2},
  }};
  for (const DomainBoundaryCase &domain_case : domain_cases) {
    CAPTURE(static_cast<int>(domain_case.id));
    CAPTURE(domain_case.operands[0]);
    CAPTURE(domain_case.operands[1]);
    std::vector<Value> scalar_args;
    std::vector<Value> vector_args;
    for (std::size_t index = 0; index < domain_case.arity; ++index) {
      scalar_args.push_back(make_int_value(domain_case.operands[index]));
      vector_args.push_back(int_vector({domain_case.operands[index]}));
    }

    EvaluationResources scalar_resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext scalar_context{scalar_resources, 0};
    const PrimitiveApplicationResult scalar_result = apply_primitive(
        scalar_context, matrix_descriptor(domain_case.id), scalar_args,
        matrix_location);
    CHECK_FALSE(scalar_result.ok);
    CHECK(scalar_result.error.kind == ErrorKind::domain_error);
    CHECK_FALSE(scalar_result.error.element_index.has_value());
    CHECK(scalar_context.scalar_kernel_invocations == 1);

    EvaluationResources vector_resources =
        make_trusted_local_resources({std::nullopt});
    PrimitiveApplicationContext vector_context{vector_resources, 0};
    const PrimitiveApplicationResult vector_result = apply_primitive(
        vector_context, matrix_descriptor(domain_case.id), vector_args,
        matrix_location);
    CHECK_FALSE(vector_result.ok);
    CHECK(vector_result.error.kind == ErrorKind::domain_error);
    REQUIRE(vector_result.error.element_index.has_value());
    CHECK(*vector_result.error.element_index == 0);
    CHECK(vector_context.scalar_kernel_invocations == 1);
    CHECK(vector_resources.live_evaluation_bytes == 0);
  }
}

} // TEST_SUITE

} // namespace bennu
