#include "bennu/evaluator.hpp"
#include "bennu/c_emitter.hpp"

#include "doctest/doctest.h"

#include <initializer_list>
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
