#include "bennu/evaluator.hpp"
#include "bennu/c_emitter.hpp"

#include "doctest/doctest.h"

#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>

namespace {

std::string formatted(bennu::Value &value) {
  const bennu::ValueFormattingResult result = bennu::format_value(value);
  CHECK(result.ok);
  return result.formatted;
}

void destroy_program(bennu::ProgramResult &result) {
  for (bennu::Value &value : result.values) {
    bennu::destroy_value(value);
  }
  result.values.clear();
}

void check_parameter_error(std::string_view source,
                           bennu::ParameterErrorReason reason,
                           std::size_t primary_begin,
                           std::size_t primary_end,
                           std::size_t related_begin,
                           std::size_t related_end) {
  bennu::ProgramResult result = bennu::evaluate_source(source);
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error.parameter.has_value());
  CHECK(result.error.parameter->reason == reason);
  CHECK(result.error.parameter->primary_span.begin.offset == primary_begin);
  CHECK(result.error.parameter->primary_span.end.offset == primary_end);
  REQUIRE(result.error.parameter->related_span.has_value());
  CHECK(result.error.parameter->related_span->begin.offset == related_begin);
  CHECK(result.error.parameter->related_span->end.offset == related_end);
}

} // namespace

TEST_CASE("CUTOVER-01 public evaluator accepts rewrite syntax and rejects the removed constructor spelling") {
  bennu::ValueResult rewritten = bennu::evaluate_expression("add[1 2.5]");
  REQUIRE(rewritten.ok);
  CHECK(formatted(rewritten.value) == "3.5");
  bennu::destroy_value(rewritten.value);

  const std::string removed_constructor = std::string("io") + "ata 3";
  bennu::ValueResult legacy =
      bennu::evaluate_expression(removed_constructor);
  REQUIRE_FALSE(legacy.ok);
  CHECK(legacy.error.kind == bennu::ErrorKind::unknown_name);
  CHECK(legacy.error.location.offset == 1U);
  CHECK(legacy.error.location.line == 1U);
  CHECK(legacy.error.location.column == 1U);
}

TEST_CASE("CUTOVER-02 public evaluator exposes exactly the rewrite primitives") {
  struct Fixture {
    const char *source;
    const char *expected;
  };
  const Fixture fixtures[] = {
      {"inc 5", "6"},
      {"inc[5]", "6"},
      {"inc inc 5", "7"},
      {"dec 5", "4"},
      {"neg[-3]", "3"},
      {"abs[-2.5]", "2.5"},
      {"sub[(10 20) 0.5]", "(9.5 19.5)"},
      {"mul[2 (3 4)]", "(6 8)"},
      {"add[10 (1 2 3)]", "(11 12 13)"},
      {"equals[2 (1 2 3 2)]", "(false true false true)"},
      {"not[(false true)]", "(true false)"},
      {"iota[3]", "(1 2 3)"},
      {"iota 0", "()"},
      {"add[Int() 0.5]", "()"},
  };
  for (const Fixture &fixture : fixtures) {
    bennu::ValueResult result = bennu::evaluate_expression(fixture.source);
    REQUIRE(result.ok);
    CHECK(formatted(result.value) == fixture.expected);
    bennu::destroy_value(result.value);
  }
  for (const char *unsupported : {"length[(1 2)]", "divide[4 2]"}) {
    bennu::ValueResult result = bennu::evaluate_expression(unsupported);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == bennu::ErrorKind::unknown_name);
    CHECK(result.error.location.column == 1U);
  }
}

TEST_CASE("CUTOVER-03 public APIs preserve structured located rewrite errors") {
  bennu::ValueResult type = bennu::evaluate_expression("add[1 true]");
  REQUIRE_FALSE(type.ok);
  CHECK(type.error.kind == bennu::ErrorKind::type_mismatch);
  CHECK(type.error.location.offset == 7U);
  CHECK(type.error.location.line == 1U);
  CHECK(type.error.location.column == 7U);
  REQUIRE(type.error.primitive.has_value());
  CHECK(type.error.primitive->name == "add");
  CHECK(type.error.argument_position == 2U);
  REQUIRE(type.error.type.has_value());
  CHECK_FALSE(type.error.message.empty());

  bennu::ValueResult shape =
      bennu::evaluate_expression("add[(1 2) (3)]");
  REQUIRE_FALSE(shape.ok);
  CHECK(shape.error.kind == bennu::ErrorKind::shape_mismatch);
  CHECK(shape.error.location.column == 11U);
  CHECK(shape.error.argument_position == 2U);
  REQUIRE(shape.error.shape.has_value());
  CHECK(shape.error.shape->expected == std::vector<std::size_t>{2U});
  CHECK(shape.error.shape->actual == std::vector<std::size_t>{1U});

  bennu::ValueResult domain =
      bennu::evaluate_expression("inc[9223372036854775807]");
  REQUIRE_FALSE(domain.ok);
  CHECK(domain.error.kind == bennu::ErrorKind::domain_error);
  REQUIRE(domain.error.domain.has_value());
  CHECK(domain.error.domain->reason ==
        bennu::DomainErrorReason::integer_overflow);
  CHECK(domain.error.location.column == 1U);

  bennu::ValueResult lifted_domain = bennu::evaluate_expression(
      "mul[(1 3037000500) (1 3037000500)]");
  REQUIRE_FALSE(lifted_domain.ok);
  CHECK(lifted_domain.error.kind == bennu::ErrorKind::domain_error);
  REQUIRE(lifted_domain.error.domain.has_value());
  CHECK(lifted_domain.error.domain->reason ==
        bennu::DomainErrorReason::integer_overflow);
  REQUIRE(lifted_domain.error.element_index.has_value());
  CHECK(*lifted_domain.error.element_index == 1U);
  REQUIRE(lifted_domain.error.primitive.has_value());
  CHECK(lifted_domain.error.primitive->name == "mul");
  CHECK(lifted_domain.error.domain->operands.size() == 2U);
}

// TEST-ID: CHECKED-ARITHMETIC-STRUCTURED-EVALUATOR
TEST_CASE("checked arithmetic evaluator errors preserve every structured overflow field") {
  struct OverflowCase {
    const char *name;
    bennu::PrimitiveId id;
    const char *scalar_source;
    const char *lifted_source;
    std::array<std::int64_t, 2> operands;
    std::size_t arity;
  };
  constexpr std::array<OverflowCase, 5> cases{{
      {"dec", bennu::PrimitiveId::dec, "dec[-9223372036854775808]",
       "dec[(0 -9223372036854775808 -9223372036854775808)]",
       {INT64_MIN, 0}, 1U},
      {"neg", bennu::PrimitiveId::neg, "neg[-9223372036854775808]",
       "neg[(0 -9223372036854775808 -9223372036854775808)]",
       {INT64_MIN, 0}, 1U},
      {"abs", bennu::PrimitiveId::abs, "abs[-9223372036854775808]",
       "abs[(0 -9223372036854775808 -9223372036854775808)]",
       {INT64_MIN, 0}, 1U},
      {"sub", bennu::PrimitiveId::sub, "sub[-9223372036854775808 1]",
       "sub[(0 -9223372036854775808 -9223372036854775808) 1]",
       {INT64_MIN, 1}, 2U},
      {"mul", bennu::PrimitiveId::mul, "mul[-9223372036854775808 -1]",
       "mul[(1 -9223372036854775808 -9223372036854775808) -1]",
       {INT64_MIN, -1}, 2U},
  }};

  for (const OverflowCase &overflow_case : cases) {
    CAPTURE(std::string(overflow_case.name));
    for (const bool lifted : {false, true}) {
      CAPTURE(lifted);
      bennu::ValueResult result = bennu::evaluate_expression(
          lifted ? overflow_case.lifted_source : overflow_case.scalar_source);
      REQUIRE_FALSE(result.ok);
      CHECK(result.error.kind == bennu::ErrorKind::domain_error);
      REQUIRE(result.error.primitive.has_value());
      REQUIRE(result.error.primitive->id.has_value());
      CHECK(*result.error.primitive->id == overflow_case.id);
      CHECK(result.error.primitive->name == overflow_case.name);
      REQUIRE(result.error.domain.has_value());
      CHECK(result.error.domain->reason ==
            bennu::DomainErrorReason::integer_overflow);
      REQUIRE(result.error.domain->signature.parameter_types.size() ==
              overflow_case.arity);
      CHECK(result.error.domain->signature.result_type ==
            bennu::ScalarType::integer);
      REQUIRE(result.error.domain->operands.size() == overflow_case.arity);
      for (std::size_t index = 0; index < overflow_case.arity; ++index) {
        CHECK(result.error.domain->signature.parameter_types[index] ==
              bennu::ScalarType::integer);
        CHECK(result.error.domain->operands[index].type ==
              bennu::ScalarType::integer);
        CHECK(result.error.domain->operands[index].integer ==
              overflow_case.operands[index]);
      }
      if (lifted) {
        REQUIRE(result.error.element_index.has_value());
        CHECK(*result.error.element_index == 1U);
      } else {
        CHECK_FALSE(result.error.element_index.has_value());
      }
      CHECK(result.error.location.offset == 1U);
      CHECK(result.error.location.line == 1U);
      CHECK(result.error.location.column == 1U);
      CHECK(result.value.container == bennu::ContainerKind::scalar);
      CHECK(result.value.vector.booleans.get() == nullptr);
      CHECK(result.value.vector.integers.get() == nullptr);
      CHECK(result.value.vector.doubles.get() == nullptr);
    }
  }

  bennu::ProgramResult transactional = bennu::evaluate_source(
      "dec[(1 2)]\n"
      "mul[(1 -9223372036854775808 -9223372036854775808) -1]\n"
      "abs[(3 4)]\n");
  REQUIRE_FALSE(transactional.ok);
  CHECK(transactional.values.empty());
  REQUIRE(transactional.error.element_index.has_value());
  CHECK(*transactional.error.element_index == 1U);
  CHECK(transactional.error.location.offset == 12U);
  CHECK(transactional.error.location.line == 2U);
  CHECK(transactional.error.location.column == 1U);
}

TEST_CASE("CUTOVER-04 public source evaluation is transactional") {
  bennu::ProgramResult failed = bennu::evaluate_source(
      "inc 5\nadd[(1 2) (3)]\nnot true\n");
  CHECK_FALSE(failed.ok);
  CHECK(failed.values.empty());
  CHECK(failed.error.kind == bennu::ErrorKind::shape_mismatch);
  CHECK(failed.error.location.line == 2U);
  CHECK(failed.error.location.column == 11U);

  bennu::ProgramResult succeeded = bennu::evaluate_source(
      "inc 5\nadd[(7 -3 11 0) 0.5]\nnot true\n");
  REQUIRE(succeeded.ok);
  REQUIRE(succeeded.values.size() == 3U);
  CHECK(formatted(succeeded.values[0]) == "6");
  CHECK(formatted(succeeded.values[1]) == "(7.5 -2.5 11.5 0.5)");
  CHECK(formatted(succeeded.values[2]) == "false");
  destroy_program(succeeded);
}

TEST_CASE("CUTOVER-05 public emitter lowers arbitrary typed vector contents") {
  const std::string source =
      "inc[(7 -3 11 0)]\n"
      "dec[(7 -3 11 0)]\n"
      "neg[-3.5]\n"
      "abs[-4]\n"
      "sub[(7 -3 11 0) 2]\n"
      "mul[(7 -3 11 0) 2.0]\n"
      "equals[true (false true)]\n"
      "add[Int() 0.5]\n"
      "iota[3]\n";
  const bennu::CEmissionResult emitted = bennu::emit_c_source(source);
  REQUIRE(emitted.ok);
  CHECK(emitted.source.find("BENNU_IMPL_INC_INT") != std::string::npos);
  CHECK(emitted.source.find("BENNU_IMPL_DEC_INT") != std::string::npos);
  CHECK(emitted.source.find("BENNU_IMPL_NEG_DOUBLE") != std::string::npos);
  CHECK(emitted.source.find("BENNU_IMPL_ABS_INT") != std::string::npos);
  CHECK(emitted.source.find("BENNU_IMPL_SUB_INT") != std::string::npos);
  CHECK(emitted.source.find("BENNU_IMPL_MUL_DOUBLE") != std::string::npos);
  CHECK(emitted.source.find("BENNU_IMPL_EQUALS_BOOL") != std::string::npos);
  CHECK(emitted.source.find("BENNU_IMPL_ADD_DOUBLE") != std::string::npos);
  CHECK(emitted.source.find("BENNU_IMPL_IOTA_INT") != std::string::npos);
  CHECK(emitted.source.find("bennu_literal_0") != std::string::npos);
  CHECK(emitted.source.find("INT64_C(7), (-INT64_C(3)), INT64_C(11), INT64_C(0)") !=
        std::string::npos);

  const bennu::CEmissionResult failed =
      bennu::emit_c_source("inc 5\nadd[1 true]\n");
  CHECK_FALSE(failed.ok);
  CHECK(failed.source.empty());
  CHECK(failed.error.kind == bennu::ErrorKind::type_mismatch);
  CHECK(failed.error.location.line == 2U);
  CHECK(failed.error.location.column == 7U);
  REQUIRE(failed.error.type.has_value());
}

TEST_CASE("CUTOVER-06 trusted local public evaluation omits arbitrary policy caps") {
  bennu::ValueResult result = bennu::evaluate_expression("iota[1000001]");
  REQUIRE(result.ok);
  std::size_t length = 0U;
  REQUIRE(bennu::value_length(result.value, length).ok);
  CHECK(length == 1000001U);
  const bennu::ScalarProjectionResult last =
      bennu::project_scalar(result.value, length - 1U);
  REQUIRE(last.ok);
  CHECK(last.value.integer == 1000001);
  bennu::destroy_value(result.value);
}

TEST_CASE("CUTOVER-07 explicit bounded profile agrees across public evaluator and emitter") {
  const bennu::EvaluationConfiguration exact{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::size_t{16U},
                            std::size_t{2U}}};
  bennu::ValueResult evaluated =
      bennu::evaluate_expression("inc[inc[(1)]]", exact);
  REQUIRE(evaluated.ok);
  CHECK(formatted(evaluated.value) == "(3)");
  bennu::destroy_value(evaluated.value);

  const bennu::CEmissionResult emitted =
      bennu::emit_c_source("inc[inc[(1)]]", exact);
  REQUIRE(emitted.ok);
  CHECK(emitted.source.find("1, 1, 1, 8U, 16U, 2U") !=
        std::string::npos);

  const bennu::EvaluationConfiguration one_past{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::size_t{16U},
                            std::size_t{1U}}};
  bennu::ValueResult refused =
      bennu::evaluate_expression("inc[inc[(1)]]", one_past);
  REQUIRE_FALSE(refused.ok);
  CHECK(refused.error.kind == bennu::ErrorKind::resource_error);
  REQUIRE(refused.error.resource.has_value());
  CHECK(refused.error.resource->reason ==
        bennu::ResourceErrorReason::profile_limit);
  CHECK(refused.error.resource->limit_kind ==
        bennu::ResourceLimitKind::max_work_units);
  CHECK(refused.error.resource->configured_limit == 1U);
  CHECK(refused.error.resource->usage_before == 1U);
  CHECK(refused.error.resource->refused_charge == 1U);

  const bennu::CEmissionResult refused_emission =
      bennu::emit_c_source("inc[inc[(1)]]", one_past);
  REQUIRE(refused_emission.ok);
  CHECK_FALSE(refused_emission.source.empty());
  CHECK(refused_emission.source.find("BENNU_PROFILE_BOUNDED_V1") !=
        std::string::npos);
  CHECK(refused_emission.source.find("BENNU_LIMIT_MAX_WORK_UNITS") !=
        std::string::npos);
  CHECK(refused_emission.source.find("bennu_source_location(5U, 1U, 5U)") !=
        std::string::npos);
  CHECK(refused_emission.source.find("BENNU_FAILURE_INTERNAL") !=
        std::string::npos);
}

TEST_CASE("PUBLIC-RESOURCE-01 allocation injection reaches evaluation and generated runtime") {
  const bennu::EvaluationConfiguration injected{
      bennu::ExecutionProfile::trusted_local_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      bennu::AllocationFailureInjection{std::size_t{0U}}};

  bennu::ValueResult evaluated = bennu::evaluate_expression("iota[2]", injected);
  REQUIRE_FALSE(evaluated.ok);
  CHECK(evaluated.error.kind == bennu::ErrorKind::resource_error);
  REQUIRE(evaluated.error.resource.has_value());
  CHECK(evaluated.error.resource->reason ==
        bennu::ResourceErrorReason::allocation_unavailable);

  const bennu::CEmissionResult emitted =
      bennu::emit_c_source("iota[2]", injected);
  REQUIRE(emitted.ok);
  CHECK(emitted.source.find("0U, 0U, 0U, 1, 0U, BENNU_FAILURE_NONE") !=
        std::string::npos);
}

TEST_CASE("LOWERING-01 emission defers value-dependent failures to runtime") {
  const bennu::CEmissionResult domain =
      bennu::emit_c_source("inc[9223372036854775807]");
  REQUIRE(domain.ok);
  CHECK(domain.source.find("BENNU_IMPL_INC_INT") != std::string::npos);

  const bennu::CEmissionResult dynamic_shape =
      bennu::emit_c_source("add[iota[2] iota[3]]");
  REQUIRE(dynamic_shape.ok);
  CHECK(dynamic_shape.source.find("BENNU_IMPL_ADD_INT") != std::string::npos);

  const bennu::CEmissionResult static_shape =
      bennu::emit_c_source("add[(1 2) (3)]");
  CHECK_FALSE(static_shape.ok);
  CHECK(static_shape.error.kind == bennu::ErrorKind::shape_mismatch);
  CHECK(static_shape.error.argument_position == 2U);

  const bennu::CEmissionResult static_container =
      bennu::emit_c_source("iota[(1 2)]");
  CHECK_FALSE(static_container.ok);
  CHECK(static_container.error.kind == bennu::ErrorKind::type_mismatch);
  CHECK(static_container.error.argument_position == 1U);
}

TEST_CASE("PARG-004-TYPED-API") {
  std::array<bennu::Value, 4> arguments{
      bennu::make_bool_value(true), bennu::make_int_value(4),
      bennu::make_double_value(0.5), bennu::make_double_value(-0.0)};

  bennu::ProgramResult result = bennu::evaluate_source(
      "parameters[enabled Bool n Int delta Double signed_zero Double]\n"
      "not[enabled]\n"
      "add[n delta]\n"
      "signed_zero\n",
      std::span<const bennu::Value>(arguments));

  REQUIRE(result.ok);
  REQUIRE(result.values.size() == 3U);
  bennu::ProgramResult literals =
      bennu::evaluate_source("not[true]\nadd[4 0.5]\n-0.0\n");
  REQUIRE(literals.ok);
  REQUIRE(literals.values.size() == result.values.size());
  CHECK(formatted(result.values[0]) == "false");
  CHECK(formatted(result.values[0]) == formatted(literals.values[0]));
  CHECK(formatted(result.values[1]) == "4.5");
  CHECK(formatted(result.values[1]) == formatted(literals.values[1]));
  CHECK(std::bit_cast<std::uint64_t>(
            result.values[2].scalar.double_precision) ==
        UINT64_C(0x8000000000000000));
  CHECK(std::bit_cast<std::uint64_t>(
            result.values[2].scalar.double_precision) ==
        std::bit_cast<std::uint64_t>(
            literals.values[2].scalar.double_precision));
  CHECK(arguments[0].scalar.boolean);
  CHECK(arguments[1].scalar.integer == 4);
  CHECK(std::bit_cast<std::uint64_t>(
            arguments[3].scalar.double_precision) ==
        UINT64_C(0x8000000000000000));
  arguments[3] = bennu::make_double_value(9.0);
  CHECK(std::bit_cast<std::uint64_t>(
            result.values[2].scalar.double_precision) ==
        UINT64_C(0x8000000000000000));
  destroy_program(literals);
  destroy_program(result);
}

TEST_CASE("PARG-009-DYNAMIC-IOTA-SHAPE") {
  std::array<bennu::Value, 3> successful_arguments{
      bennu::make_int_value(3), bennu::make_int_value(2),
      bennu::make_double_value(0.5)};
  bennu::ProgramResult successful = bennu::evaluate_source(
      "parameters[n Int x Int delta Double]\n"
      "iota[n]\n"
      "add[x delta]\n",
      std::span<const bennu::Value>(successful_arguments));
  REQUIRE(successful.ok);
  REQUIRE(successful.values.size() == 2U);
  CHECK(formatted(successful.values[0]) == "(1 2 3)");
  CHECK(formatted(successful.values[1]) == "2.5");
  destroy_program(successful);

  std::array<bennu::Value, 2> shape_arguments{bennu::make_int_value(2),
                                              bennu::make_int_value(3)};
  bennu::ProgramResult shape = bennu::evaluate_source(
      "parameters[left Int right Int]\nadd[iota[left] iota[right]]\n",
      std::span<const bennu::Value>(shape_arguments));
  REQUIRE_FALSE(shape.ok);
  CHECK(shape.error.kind == bennu::ErrorKind::shape_mismatch);
  CHECK(shape.error.argument_position == 2U);

  std::array<bennu::Value, 1> overflow_argument{bennu::make_int_value(
      std::numeric_limits<std::int64_t>::max())};
  bennu::ProgramResult overflow = bennu::evaluate_source(
      "parameters[n Int]\ninc[n]\n",
      std::span<const bennu::Value>(overflow_argument));
  REQUIRE_FALSE(overflow.ok);
  CHECK(overflow.error.kind == bennu::ErrorKind::domain_error);
  REQUIRE(overflow.error.domain.has_value());
  CHECK(overflow.error.domain->reason ==
        bennu::DomainErrorReason::integer_overflow);

  const bennu::EvaluationConfiguration bounded{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::nullopt, std::nullopt}};
  std::array<bennu::Value, 1> resource_argument{bennu::make_int_value(2)};
  bennu::ProgramResult resource = bennu::evaluate_source(
      "parameters[n Int]\niota[n]\n",
      std::span<const bennu::Value>(resource_argument), bounded);
  REQUIRE_FALSE(resource.ok);
  CHECK(resource.error.kind == bennu::ErrorKind::resource_error);
  REQUIRE(resource.error.resource.has_value());
  CHECK(resource.error.resource->reason ==
        bennu::ResourceErrorReason::profile_limit);
  CHECK(resource.error.resource->limit_kind ==
        bennu::ResourceLimitKind::max_vector_bytes);
}

TEST_CASE("PARG-005-ARGUMENT-ERROR reports exact count context before value inspection") {
  bennu::ProgramResult missing =
      bennu::evaluate_source("parameters[n Int]\nn\n");
  REQUIRE_FALSE(missing.ok);
  CHECK(missing.error.kind == bennu::ErrorKind::argument_error);
  REQUIRE(missing.error.argument.has_value());
  CHECK(missing.error.argument->reason == bennu::ArgumentErrorReason::missing);
  CHECK(missing.error.argument->required_count == 1U);
  CHECK(missing.error.argument->supplied_count == 0U);
  CHECK(missing.error.argument->position == 1U);
  CHECK(missing.error.argument->parameter_name == "n");
  CHECK(missing.error.argument->expected_type == bennu::ScalarType::integer);
  REQUIRE(missing.error.argument->declaration_span.has_value());
  CHECK(missing.error.argument->declaration_span->begin.offset == 12U);
  CHECK(missing.error.argument->declaration_span->end.offset == 17U);
  CHECK_FALSE(missing.error.argument->actual_container.has_value());
  CHECK_FALSE(missing.error.argument->actual_type.has_value());
  CHECK_FALSE(missing.error.argument->invalid_value_invariant.has_value());

  bennu::Value invalid = bennu::make_int_value(1);
  invalid.container = static_cast<bennu::ContainerKind>(99);
  const std::array<bennu::Value, 1> extra{std::move(invalid)};
  bennu::ProgramResult extra_result = bennu::evaluate_source(
      "parameters[]\n1\n", std::span<const bennu::Value>(extra));
  REQUIRE_FALSE(extra_result.ok);
  CHECK(extra_result.error.kind == bennu::ErrorKind::argument_error);
  REQUIRE(extra_result.error.argument.has_value());
  CHECK(extra_result.error.argument->reason == bennu::ArgumentErrorReason::extra);
  CHECK(extra_result.error.argument->required_count == 0U);
  CHECK(extra_result.error.argument->supplied_count == 1U);
  CHECK(extra_result.error.argument->position == 1U);
  CHECK_FALSE(extra_result.error.argument->parameter_name.has_value());
  CHECK_FALSE(extra_result.error.argument->expected_type.has_value());
  CHECK_FALSE(extra_result.error.argument->declaration_span.has_value());
  CHECK_FALSE(extra_result.error.argument->actual_container.has_value());
}

TEST_CASE("PARG-005-ARGUMENT-ERROR") {
  const std::string source = "parameters[n Int]\nn\n";

  bennu::Value unknown_container = bennu::make_int_value(1);
  unknown_container.container = static_cast<bennu::ContainerKind>(99);
  std::array<bennu::Value, 1> unknown_container_arguments{
      std::move(unknown_container)};
  bennu::ProgramResult unknown_container_result = bennu::evaluate_source(
      source, std::span<const bennu::Value>(unknown_container_arguments));
  REQUIRE_FALSE(unknown_container_result.ok);
  REQUIRE(unknown_container_result.error.argument.has_value());
  CHECK(unknown_container_result.error.argument->reason ==
        bennu::ArgumentErrorReason::invalid_typed_value);
  CHECK(unknown_container_result.error.argument->actual_container ==
        bennu::ArgumentContainer::unknown);
  CHECK_FALSE(unknown_container_result.error.argument->actual_type.has_value());
  CHECK(unknown_container_result.error.argument->invalid_value_invariant ==
        bennu::ValueInvariant::unknown_container);

  bennu::Value vector = bennu::make_int_value(0);
  vector.container = bennu::ContainerKind::vector;
  vector.scalar =
      bennu::ScalarValue{bennu::ScalarType::boolean, false, 0, 0.0};
  vector.vector.element_type = static_cast<bennu::ScalarType>(99);
  std::array<bennu::Value, 1> vector_arguments{std::move(vector)};
  bennu::ProgramResult vector_result = bennu::evaluate_source(
      source, std::span<const bennu::Value>(vector_arguments));
  REQUIRE_FALSE(vector_result.ok);
  REQUIRE(vector_result.error.argument.has_value());
  CHECK(vector_result.error.argument->reason ==
        bennu::ArgumentErrorReason::container_mismatch);
  CHECK(vector_result.error.argument->actual_container ==
        bennu::ArgumentContainer::vector);
  CHECK_FALSE(vector_result.error.argument->actual_type.has_value());
  CHECK_FALSE(vector_result.error.argument->invalid_value_invariant.has_value());

  bennu::Value unknown_type = bennu::make_int_value(1);
  unknown_type.scalar.type = static_cast<bennu::ScalarType>(99);
  std::array<bennu::Value, 1> unknown_type_arguments{std::move(unknown_type)};
  bennu::ProgramResult unknown_type_result = bennu::evaluate_source(
      source, std::span<const bennu::Value>(unknown_type_arguments));
  REQUIRE_FALSE(unknown_type_result.ok);
  REQUIRE(unknown_type_result.error.argument.has_value());
  CHECK(unknown_type_result.error.argument->reason ==
        bennu::ArgumentErrorReason::invalid_typed_value);
  CHECK(unknown_type_result.error.argument->actual_container ==
        bennu::ArgumentContainer::scalar);
  CHECK(unknown_type_result.error.argument->actual_type ==
        bennu::ArgumentScalarType::unknown);
  CHECK(unknown_type_result.error.argument->invalid_value_invariant ==
        bennu::ValueInvariant::unknown_scalar_type);

  bennu::Value inactive = bennu::make_int_value(1);
  inactive.scalar.boolean = true;
  std::array<bennu::Value, 1> inactive_arguments{std::move(inactive)};
  bennu::ProgramResult inactive_result = bennu::evaluate_source(
      source, std::span<const bennu::Value>(inactive_arguments));
  REQUIRE_FALSE(inactive_result.ok);
  REQUIRE(inactive_result.error.argument.has_value());
  CHECK(inactive_result.error.argument->actual_type ==
        bennu::ArgumentScalarType::integer);
  CHECK(inactive_result.error.argument->invalid_value_invariant ==
        bennu::ValueInvariant::inactive_scalar_field);

  bennu::Value inactive_vector_state = bennu::make_int_value(1);
  inactive_vector_state.vector.element_type = bennu::ScalarType::integer;
  std::array<bennu::Value, 1> inactive_vector_state_arguments{
      std::move(inactive_vector_state)};
  bennu::ProgramResult inactive_vector_state_result = bennu::evaluate_source(
      source, std::span<const bennu::Value>(inactive_vector_state_arguments));
  REQUIRE_FALSE(inactive_vector_state_result.ok);
  REQUIRE(inactive_vector_state_result.error.argument.has_value());
  CHECK(inactive_vector_state_result.error.argument->actual_type ==
        bennu::ArgumentScalarType::integer);
  CHECK(inactive_vector_state_result.error.argument->invalid_value_invariant ==
        bennu::ValueInvariant::inactive_scalar_field);

  bennu::Value noncanonical = bennu::make_double_value(0.0);
  noncanonical.scalar.double_precision =
      std::bit_cast<double>(UINT64_C(0x7ff0000000000001));
  std::array<bennu::Value, 1> noncanonical_arguments{std::move(noncanonical)};
  bennu::ProgramResult noncanonical_result = bennu::evaluate_source(
      "parameters[x Double]\nx\n",
      std::span<const bennu::Value>(noncanonical_arguments));
  REQUIRE_FALSE(noncanonical_result.ok);
  REQUIRE(noncanonical_result.error.argument.has_value());
  CHECK(noncanonical_result.error.argument->actual_type ==
        bennu::ArgumentScalarType::double_precision);
  CHECK(noncanonical_result.error.argument->invalid_value_invariant ==
        bennu::ValueInvariant::noncanonical_nan);

  std::array<bennu::Value, 1> wrong_type{bennu::make_bool_value(true)};
  bennu::ProgramResult wrong_type_result = bennu::evaluate_source(
      source, std::span<const bennu::Value>(wrong_type));
  REQUIRE_FALSE(wrong_type_result.ok);
  REQUIRE(wrong_type_result.error.argument.has_value());
  CHECK(wrong_type_result.error.argument->reason ==
        bennu::ArgumentErrorReason::type_mismatch);
  CHECK(wrong_type_result.error.argument->actual_type ==
        bennu::ArgumentScalarType::boolean);
  CHECK_FALSE(wrong_type_result.error.argument->invalid_value_invariant.has_value());

  bennu::Value invalid_first = bennu::make_int_value(1);
  invalid_first.scalar.boolean = true;
  bennu::Value invalid_second = bennu::make_int_value(2);
  invalid_second.scalar.boolean = true;
  std::array<bennu::Value, 2> two_invalid{
      std::move(invalid_first), std::move(invalid_second)};
  bennu::ProgramResult first_invalid = bennu::evaluate_source(
      "parameters[first Int second Int]\nadd[first second]\n",
      std::span<const bennu::Value>(two_invalid));
  REQUIRE_FALSE(first_invalid.ok);
  REQUIRE(first_invalid.error.argument.has_value());
  CHECK(first_invalid.error.argument->position == 1U);
  CHECK(first_invalid.error.argument->parameter_name == "first");

  two_invalid[0] = bennu::make_int_value(1);
  bennu::ProgramResult second_invalid = bennu::evaluate_source(
      "parameters[first Int second Int]\nadd[first second]\n",
      std::span<const bennu::Value>(two_invalid));
  REQUIRE_FALSE(second_invalid.ok);
  REQUIRE(second_invalid.error.argument.has_value());
  CHECK(second_invalid.error.argument->position == 2U);
  CHECK(second_invalid.error.argument->parameter_name == "second");
}

TEST_CASE("PARG-001-HEADER") {
  std::array<bennu::Value, 2> multiline_arguments{
      bennu::make_int_value(3), bennu::make_bool_value(true)};
  bennu::ProgramResult multiline = bennu::evaluate_source(
      " \r\nparameters[\r\n n Int\r\n enabled Bool\r\n]\r\n"
      "add[n n]\r\nnot[enabled]\r\n",
      std::span<const bennu::Value>(multiline_arguments));
  REQUIRE(multiline.ok);
  REQUIRE(multiline.values.size() == 2U);
  CHECK(formatted(multiline.values[0]) == "6");
  CHECK(formatted(multiline.values[1]) == "false");
  destroy_program(multiline);

  check_parameter_error("parameters",
                        bennu::ParameterErrorReason::expected_header_open,
                        11U, 11U, 1U, 11U);
  check_parameter_error("parameters]",
                        bennu::ParameterErrorReason::expected_header_open,
                        11U, 12U, 1U, 11U);
  check_parameter_error("parameters [n Int]",
                        bennu::ParameterErrorReason::expected_header_open,
                        11U, 12U, 1U, 11U);
  check_parameter_error("parameters[",
                        bennu::ParameterErrorReason::missing_header_close,
                        12U, 12U, 11U, 12U);
  check_parameter_error("parameters[n",
                        bennu::ParameterErrorReason::expected_parameter_type,
                        13U, 13U, 12U, 13U);
  check_parameter_error("parameters[n]",
                        bennu::ParameterErrorReason::expected_parameter_type,
                        13U, 13U, 12U, 13U);
  check_parameter_error("parameters[Int]",
                        bennu::ParameterErrorReason::expected_parameter_name,
                        12U, 15U, 11U, 12U);
  check_parameter_error("parameters[,]",
                        bennu::ParameterErrorReason::unexpected_header_token,
                        12U, 13U, 11U, 12U);
  check_parameter_error("parameters[n Integer]",
                        bennu::ParameterErrorReason::unexpected_header_token,
                        14U, 21U, 12U, 13U);
  check_parameter_error("parameters[n Int",
                        bennu::ParameterErrorReason::missing_header_close,
                        17U, 17U, 11U, 12U);
  check_parameter_error("parameters[]x",
                        bennu::ParameterErrorReason::trailing_header_bytes,
                        13U, 14U, 1U, 13U);
  check_parameter_error("parameters[]\nparameters[]",
                        bennu::ParameterErrorReason::second_parameter_header,
                        14U, 24U, 1U, 13U);
  check_parameter_error("1\nparameters[]",
                        bennu::ParameterErrorReason::parameter_header_after_root,
                        3U, 13U, 1U, 2U);
  check_parameter_error("parameters[]\n1\nparameters[]",
                        bennu::ParameterErrorReason::second_parameter_header,
                        16U, 26U, 1U, 13U);
}

TEST_CASE("PARG-001-NAMES") {
  bennu::ProgramResult duplicate =
      bennu::evaluate_source("parameters[n Int n Bool]\nn\n");
  REQUIRE_FALSE(duplicate.ok);
  CHECK(duplicate.error.kind ==
        bennu::ErrorKind::invalid_parameter_declaration);
  REQUIRE(duplicate.error.parameter.has_value());
  CHECK(duplicate.error.parameter->reason ==
        bennu::ParameterErrorReason::duplicate_parameter_name);
  CHECK(duplicate.error.parameter->primary_span.begin.offset == 18U);
  REQUIRE(duplicate.error.parameter->related_span.has_value());
  CHECK(duplicate.error.parameter->related_span->begin.offset == 12U);

  for (std::string_view name : {"parameters", "fanout", "true", "false",
                                "inf", "nan", "inc", "add", "equals",
                                "not", "iota"}) {
    const std::string source =
        "parameters[" + std::string(name) + " Int]\n1\n";
    bennu::ProgramResult reserved = bennu::evaluate_source(source);
    INFO(std::string(name));
    REQUIRE_FALSE(reserved.ok);
    CHECK(reserved.error.kind ==
          bennu::ErrorKind::invalid_parameter_declaration);
    REQUIRE(reserved.error.parameter.has_value());
    CHECK(reserved.error.parameter->reason ==
          bennu::ParameterErrorReason::reserved_parameter_name);
  }

  bennu::ProgramResult unknown =
      bennu::evaluate_source("parameters[n Int]\nm\n",
                             std::array{bennu::make_int_value(1)});
  REQUIRE_FALSE(unknown.ok);
  CHECK(unknown.error.kind == bennu::ErrorKind::unknown_name);
  CHECK(unknown.error.location.offset == 19U);
  REQUIRE(unknown.error.primary_span.has_value());
  CHECK(unknown.error.primary_span->begin.offset == 19U);
  CHECK(unknown.error.primary_span->end.offset == 20U);

  bennu::ProgramResult bracket_call = bennu::evaluate_source(
      "parameters[n Int]\nn[1]\n", std::array{bennu::make_int_value(1)});
  REQUIRE_FALSE(bracket_call.ok);
  CHECK(bracket_call.error.kind == bennu::ErrorKind::unknown_name);
  CHECK(bracket_call.error.location.offset == 19U);

  bennu::ProgramResult prefix_call = bennu::evaluate_source(
      "parameters[n Int]\nn 1\n", std::array{bennu::make_int_value(1)});
  REQUIRE_FALSE(prefix_call.ok);
  CHECK(prefix_call.error.kind == bennu::ErrorKind::unknown_name);
  CHECK(prefix_call.error.location.offset == 19U);

  bennu::ProgramResult prefix_operand = bennu::evaluate_source(
      "parameters[n Int]\ninc n\n", std::array{bennu::make_int_value(1)});
  REQUIRE(prefix_operand.ok);
  REQUIRE(prefix_operand.values.size() == 1U);
  CHECK(formatted(prefix_operand.values[0]) == "2");
  destroy_program(prefix_operand);

  bennu::ProgramResult vector_interior =
      bennu::evaluate_source("parameters[n Int]\n(n 1)\n");
  REQUIRE_FALSE(vector_interior.ok);
  CHECK(vector_interior.error.kind == bennu::ErrorKind::syntax_error);
}

TEST_CASE("PARG-002-STATIC-ORDER") {
  bennu::ProgramResult unknown =
      bennu::evaluate_source("parameters[n Int]\nm\n");
  REQUIRE_FALSE(unknown.ok);
  CHECK(unknown.error.kind == bennu::ErrorKind::unknown_name);

  bennu::ProgramResult arity =
      bennu::evaluate_source("parameters[n Int]\ninc[1 2]\n");
  REQUIRE_FALSE(arity.ok);
  CHECK(arity.error.kind == bennu::ErrorKind::arity_error);

  bennu::ProgramResult type =
      bennu::evaluate_source("parameters[n Int]\nadd[true false]\n");
  REQUIRE_FALSE(type.ok);
  CHECK(type.error.kind == bennu::ErrorKind::type_mismatch);

  bennu::Value invalid = bennu::make_int_value(1);
  invalid.container = static_cast<bennu::ContainerKind>(99);
  std::array<bennu::Value, 1> invalid_argument{std::move(invalid)};
  bennu::ProgramResult shape_before_binding = bennu::evaluate_source(
      "parameters[n Int]\nadd[(1 2) (3 4 5)]\nn\n",
      std::span<const bennu::Value>(invalid_argument));
  REQUIRE_FALSE(shape_before_binding.ok);
  CHECK(shape_before_binding.error.kind == bennu::ErrorKind::shape_mismatch);
}

TEST_CASE("PARG-003-SHAPE-ANALYSIS") {
  bennu::ProgramResult static_mismatch = bennu::evaluate_source(
      "parameters[n Int]\nadd[(1 2) (3 4 5)]\nn\n");
  REQUIRE_FALSE(static_mismatch.ok);
  CHECK(static_mismatch.error.kind == bennu::ErrorKind::shape_mismatch);

  bennu::ProgramResult dynamic_requires_binding = bennu::evaluate_source(
      "parameters[n Int]\nadd[(1 2) iota[n]]\n");
  REQUIRE_FALSE(dynamic_requires_binding.ok);
  CHECK(dynamic_requires_binding.error.kind == bennu::ErrorKind::argument_error);

  std::array<bennu::Value, 1> matching{bennu::make_int_value(2)};
  bennu::ProgramResult dynamic_match = bennu::evaluate_source(
      "parameters[n Int]\nadd[(1 2) iota[n]]\n",
      std::span<const bennu::Value>(matching));
  REQUIRE(dynamic_match.ok);
  REQUIRE(dynamic_match.values.size() == 1U);
  CHECK(formatted(dynamic_match.values[0]) == "(2 4)");
  destroy_program(dynamic_match);

  std::array<bennu::Value, 2> different_dynamic_lengths{
      bennu::make_int_value(2), bennu::make_int_value(3)};
  bennu::ProgramResult two_dynamic = bennu::evaluate_source(
      "parameters[left Int right Int]\nadd[iota[left] iota[right]]\n",
      std::span<const bennu::Value>(different_dynamic_lengths));
  REQUIRE_FALSE(two_dynamic.ok);
  CHECK(two_dynamic.error.kind == bennu::ErrorKind::shape_mismatch);
  CHECK(two_dynamic.error.argument_position == 2U);
}

TEST_CASE("PARG-008-ZERO-ROOTS") {
  std::array<bennu::Value, 2> values{bennu::make_int_value(7),
                                     bennu::make_bool_value(false)};
  bennu::ProgramResult empty = bennu::evaluate_source(
      "parameters[n Int unused Bool]\n",
      std::span<const bennu::Value>(values));
  REQUIRE(empty.ok);
  CHECK(empty.values.empty());

  bennu::ProgramResult empty_header = bennu::evaluate_source("parameters[]\n");
  REQUIRE(empty_header.ok);
  CHECK(empty_header.values.empty());

  std::array<bennu::Value, 1> wrong_unused{bennu::make_int_value(7)};
  bennu::ProgramResult invalid_unused = bennu::evaluate_source(
      "parameters[unused Bool]\n",
      std::span<const bennu::Value>(wrong_unused));
  REQUIRE_FALSE(invalid_unused.ok);
  REQUIRE(invalid_unused.error.argument.has_value());
  CHECK(invalid_unused.error.argument->reason ==
        bennu::ArgumentErrorReason::type_mismatch);
  CHECK(invalid_unused.error.argument->position == 1U);

  std::array<bennu::Value, 2> repeated_values{bennu::make_int_value(7),
                                              bennu::make_bool_value(false)};
  bennu::ProgramResult repeated = bennu::evaluate_source(
      "parameters[n Int unused Bool]\nadd[n n]\nn\n",
      std::span<const bennu::Value>(repeated_values));
  REQUIRE(repeated.ok);
  REQUIRE(repeated.values.size() == 2U);
  CHECK(formatted(repeated.values[0]) == "14");
  CHECK(formatted(repeated.values[1]) == "7");
  destroy_program(repeated);
}

TEST_CASE("PARG-010-RUNTIME-ORDER") {
  std::array<bennu::Value, 1> maximum{bennu::make_int_value(
      std::numeric_limits<std::int64_t>::max())};
  bennu::ProgramResult first_root_domain = bennu::evaluate_source(
      "parameters[n Int]\ninc[n]\niota[n]\n",
      std::span<const bennu::Value>(maximum));
  REQUIRE_FALSE(first_root_domain.ok);
  CHECK(first_root_domain.values.empty());
  CHECK(first_root_domain.error.kind == bennu::ErrorKind::domain_error);

  std::array<bennu::Value, 3> shape_then_domain{
      bennu::make_int_value(2), bennu::make_int_value(3),
      bennu::make_int_value(std::numeric_limits<std::int64_t>::max())};
  bennu::ProgramResult first_root_shape = bennu::evaluate_source(
      "parameters[left Int right Int maximum Int]\n"
      "add[iota[left] iota[right]]\ninc[maximum]\n",
      std::span<const bennu::Value>(shape_then_domain));
  REQUIRE_FALSE(first_root_shape.ok);
  CHECK(first_root_shape.values.empty());
  CHECK(first_root_shape.error.kind == bennu::ErrorKind::shape_mismatch);

  const bennu::EvaluationConfiguration bounded{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::nullopt, std::nullopt}};
  std::array<bennu::Value, 1> three{bennu::make_int_value(3)};
  bennu::ProgramResult child_resource = bennu::evaluate_source(
      "parameters[n Int]\nadd[(1 2) iota[n]]\n",
      std::span<const bennu::Value>(three), bounded);
  REQUIRE_FALSE(child_resource.ok);
  CHECK(child_resource.values.empty());
  CHECK(child_resource.error.kind == bennu::ErrorKind::resource_error);
}

TEST_CASE("PARG-011-PROFILES") {
  const bennu::EvaluationConfiguration no_work{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::size_t{0U}}};
  std::array<bennu::Value, 1> argument{bennu::make_int_value(4)};

  bennu::ProgramResult binding_only = bennu::evaluate_source(
      "parameters[n Int]\nn\n", std::span<const bennu::Value>(argument),
      no_work);
  REQUIRE(binding_only.ok);
  REQUIRE(binding_only.values.size() == 1U);
  CHECK(formatted(binding_only.values[0]) == "4");
  destroy_program(binding_only);

  bennu::ProgramResult call_is_charged = bennu::evaluate_source(
      "parameters[n Int]\ninc[n]\n", std::span<const bennu::Value>(argument),
      no_work);
  REQUIRE_FALSE(call_is_charged.ok);
  CHECK(call_is_charged.error.kind == bennu::ErrorKind::resource_error);
  REQUIRE(call_is_charged.error.resource.has_value());
  CHECK(call_is_charged.error.resource->reason ==
        bennu::ResourceErrorReason::profile_limit);
  CHECK(call_is_charged.error.resource->limit_kind ==
        bennu::ResourceLimitKind::max_work_units);

  bennu::ProgramResult binding_failure_precedes_profile =
      bennu::evaluate_source("parameters[n Int]\ninc[n]\n",
                             std::span<const bennu::Value>{}, no_work);
  REQUIRE_FALSE(binding_failure_precedes_profile.ok);
  CHECK(binding_failure_precedes_profile.error.kind ==
        bennu::ErrorKind::argument_error);
}

TEST_CASE("Issue 46 explicitly defers parameterized C emission to Issue 48") {
  bennu::CEmissionResult emitted =
      bennu::emit_c_source("parameters[n Int]\nn\n");
  REQUIRE_FALSE(emitted.ok);
  CHECK(emitted.error.kind == bennu::ErrorKind::syntax_error);
  REQUIRE(emitted.error.parameter.has_value());
  CHECK(emitted.error.parameter->reason ==
        bennu::ParameterErrorReason::unsupported_parameterized_surface);
}

TEST_CASE("PARG-017-REGRESSION") {
  bennu::ProgramResult absent_header = bennu::evaluate_source("add[1 2]\n");
  REQUIRE(absent_header.ok);
  REQUIRE(absent_header.values.size() == 1U);
  CHECK(formatted(absent_header.values[0]) == "3");
  destroy_program(absent_header);

  bennu::ProgramResult empty_header =
      bennu::evaluate_source("parameters[]\nadd[1 2]\n");
  REQUIRE(empty_header.ok);
  REQUIRE(empty_header.values.size() == 1U);
  CHECK(formatted(empty_header.values[0]) == "3");
  destroy_program(empty_header);

  bennu::ValueResult result =
      bennu::evaluate_expression("parameters[n Int]\nn\n");
  REQUIRE_FALSE(result.ok);
  CHECK(result.error.kind == bennu::ErrorKind::syntax_error);
  REQUIRE(result.error.parameter.has_value());
  CHECK(result.error.parameter->reason ==
        bennu::ParameterErrorReason::program_only_parameter_header);
  CHECK(result.error.location.offset == 1U);

  bennu::ValueResult bare_name = bennu::evaluate_expression("name");
  REQUIRE_FALSE(bare_name.ok);
  CHECK(bare_name.error.kind == bennu::ErrorKind::unknown_name);
  CHECK(bare_name.error.location.offset == 1U);
}
