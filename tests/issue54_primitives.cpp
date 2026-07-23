#include "bennu/application.hpp"
#include "bennu/evaluator.hpp"
#include "bennu/primitive.hpp"
#include "bennu/value.hpp"

#include "doctest/doctest.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace bennu {
namespace {

std::string issue54_evaluate(std::string_view source) {
  ValueResult result = evaluate_expression(source);
  if (!result.ok) {
    CHECK(result.ok);
    return "<evaluation-error>";
  }
  const ValueFormattingResult formatted = format_value(result.value);
  if (!formatted.ok) {
    CHECK(formatted.ok);
    destroy_value(result.value);
    return "<formatting-error>";
  }
  destroy_value(result.value);
  return formatted.formatted;
}

} // namespace

TEST_SUITE("Issue #54 Boolean predicate and ordering primitives") {

TEST_CASE("ISSUE54-REGISTRY Boolean and inequality identities are registered") {
  struct ExpectedPrimitive {
    std::string_view name;
    std::size_t signature_count;
  };
  constexpr std::array<ExpectedPrimitive, 3> expected{{
      {"and", 1U},
      {"or", 1U},
      {"not_equals", 3U},
  }};

  for (const ExpectedPrimitive &primitive : expected) {
    INFO(std::string(primitive.name));
    const PrimitiveDescriptor *descriptor = find_primitive(primitive.name);
    if (descriptor == nullptr) {
      CHECK(descriptor != nullptr);
      continue;
    }
    CHECK(descriptor->lifting == LiftingMode::elementwise);
    CHECK(descriptor->signature_count == primitive.signature_count);
    CHECK(find_primitive(descriptor->id) == descriptor);
  }
  CHECK(static_cast<std::uint8_t>(PrimitiveId::logical_and) == 5U);
  CHECK(static_cast<std::uint8_t>(PrimitiveId::logical_or) == 6U);
  CHECK(static_cast<std::uint8_t>(PrimitiveId::not_equals) == 7U);
  CHECK(production_primitive_table_validation().ok);
}

TEST_CASE("ISSUE54-BOOLEAN and and or use ordinary pointwise truth tables") {
  struct BooleanCase {
    std::string_view source;
    std::string_view expected;
  };
  constexpr std::array<BooleanCase, 10> cases{{
      {"and[false false]", "false"},
      {"and[false true]", "false"},
      {"and[true false]", "false"},
      {"and[true true]", "true"},
      {"or[false false]", "false"},
      {"or[false true]", "true"},
      {"or[true false]", "true"},
      {"or[true true]", "true"},
      {"and[true (true false true)]", "(true false true)"},
      {"or[(false true false) false]", "(false true false)"},
  }};

  for (const BooleanCase &test_case : cases) {
    INFO(std::string(test_case.source));
    CHECK(issue54_evaluate(test_case.source) == std::string(test_case.expected));
  }
}

TEST_CASE("ISSUE54-NOT-EQUALS is the exact complement of selected equals") {
  constexpr std::array<std::string_view, 11> operand_pairs{{
      "true true",
      "true false",
      "0 0",
      "-9223372036854775808 9223372036854775807",
      "0.0 -0.0",
      "nan nan",
      "nan 1.0",
      "inf inf",
      "-inf inf",
      "9007199254740993 9007199254740992.0",
      "9007199254740993 9007199254740994.0",
  }};

  for (const std::string_view operands : operand_pairs) {
    INFO(std::string(operands));
    const std::string equals_source = "equals[" + std::string(operands) + "]";
    const std::string not_equals_source =
        "not_equals[" + std::string(operands) + "]";
    const std::string equals_result = issue54_evaluate(equals_source);
    const std::string not_equals_result = issue54_evaluate(not_equals_source);
    if (equals_result == "true") {
      CHECK(not_equals_result == "false");
    } else if (equals_result == "false") {
      CHECK(not_equals_result == "true");
    }
  }
  CHECK(issue54_evaluate("equals[(1 2 3) 2]") == "(false true false)");
  CHECK(issue54_evaluate("not_equals[(1 2 3) 2]") == "(true false true)");
}

TEST_CASE("ISSUE54-REGISTRY numeric predicates and ordering are registered") {
  struct ExpectedPrimitive {
    std::string_view name;
    std::size_t signature_count;
  };
  constexpr std::array<ExpectedPrimitive, 6> expected{{
      {"odd", 1U},
      {"even", 1U},
      {"is_positive", 2U},
      {"is_negative", 2U},
      {"less_than", 2U},
      {"greater_than", 2U},
  }};

  for (const ExpectedPrimitive &primitive : expected) {
    INFO(std::string(primitive.name));
    const PrimitiveDescriptor *descriptor = find_primitive(primitive.name);
    if (descriptor == nullptr) {
      CHECK(descriptor != nullptr);
      continue;
    }
    CHECK(descriptor->lifting == LiftingMode::elementwise);
    CHECK(descriptor->signature_count == primitive.signature_count);
    CHECK(find_primitive(descriptor->id) == descriptor);
  }
  CHECK(static_cast<std::uint8_t>(PrimitiveId::odd) == 8U);
  CHECK(static_cast<std::uint8_t>(PrimitiveId::even) == 9U);
  CHECK(static_cast<std::uint8_t>(PrimitiveId::is_positive) == 10U);
  CHECK(static_cast<std::uint8_t>(PrimitiveId::is_negative) == 11U);
  CHECK(static_cast<std::uint8_t>(PrimitiveId::less_than) == 12U);
  CHECK(static_cast<std::uint8_t>(PrimitiveId::greater_than) == 13U);
  CHECK(production_primitive_table_validation().ok);
}

TEST_CASE("ISSUE54-PARITY handles negative values and Int64 extrema without overflow") {
  struct PredicateCase {
    std::string_view source;
    std::string_view expected;
  };
  constexpr std::array<PredicateCase, 12> cases{{
      {"odd[0]", "false"},
      {"even[0]", "true"},
      {"odd[-3]", "true"},
      {"even[-3]", "false"},
      {"odd[9223372036854775807]", "true"},
      {"even[9223372036854775807]", "false"},
      {"odd[-9223372036854775808]", "false"},
      {"even[-9223372036854775808]", "true"},
      {"odd[(0 1 -2 -3)]", "(false true false true)"},
      {"even[(0 1 -2 -3)]", "(true false true false)"},
      {"odd[Int()]", "()"},
      {"even[Int()]", "()"},
  }};
  for (const PredicateCase &test_case : cases) {
    INFO(std::string(test_case.source));
    CHECK(issue54_evaluate(test_case.source) == std::string(test_case.expected));
  }
}

TEST_CASE("ISSUE54-SIGN handles extrema zeros infinities NaN and raw NaN normalization") {
  struct PredicateCase {
    std::string_view source;
    std::string_view expected;
  };
  constexpr std::array<PredicateCase, 18> cases{{
      {"is_positive[-9223372036854775808]", "false"},
      {"is_negative[-9223372036854775808]", "true"},
      {"is_positive[9223372036854775807]", "true"},
      {"is_negative[9223372036854775807]", "false"},
      {"is_positive[0]", "false"},
      {"is_negative[0]", "false"},
      {"is_positive[0.0]", "false"},
      {"is_negative[0.0]", "false"},
      {"is_positive[-0.0]", "false"},
      {"is_negative[-0.0]", "false"},
      {"is_positive[inf]", "true"},
      {"is_negative[inf]", "false"},
      {"is_positive[-inf]", "false"},
      {"is_negative[-inf]", "true"},
      {"is_positive[nan]", "false"},
      {"is_negative[nan]", "false"},
      {"is_positive[(-1.0 0.0 1.0 nan)]", "(false false true false)"},
      {"is_negative[(-1.0 -0.0 1.0 nan)]", "(true false false false)"},
  }};
  for (const PredicateCase &test_case : cases) {
    INFO(std::string(test_case.source));
    CHECK(issue54_evaluate(test_case.source) == std::string(test_case.expected));
  }

  const double raw_nan =
      std::bit_cast<double>(UINT64_C(0xfff123456789abcd));
  Value normalized = make_double_value(raw_nan);
  CHECK(std::bit_cast<std::uint64_t>(normalized.scalar.double_precision) ==
        UINT64_C(0x7ff8000000000000));
  destroy_value(normalized);
}

TEST_CASE("ISSUE54-ORDERING preserves written operands and IEEE unordered cases") {
  struct OrderingCase {
    std::string_view source;
    std::string_view expected;
  };
  constexpr std::array<OrderingCase, 24> cases{{
      {"less_than[1 2]", "true"},
      {"less_than[2 1]", "false"},
      {"greater_than[1 2]", "false"},
      {"greater_than[2 1]", "true"},
      {"less_than[2 2]", "false"},
      {"greater_than[2 2]", "false"},
      {"less_than[0.0 -0.0]", "false"},
      {"greater_than[-0.0 0.0]", "false"},
      {"less_than[nan 1.0]", "false"},
      {"less_than[1.0 nan]", "false"},
      {"greater_than[nan 1.0]", "false"},
      {"greater_than[1.0 nan]", "false"},
      {"less_than[-inf inf]", "true"},
      {"greater_than[inf 1.7976931348623157e308]", "true"},
      {"less_than[4.9406564584124654e-324 1.0]", "true"},
      {"greater_than[-4.9406564584124654e-324 -1.0]", "true"},
      {"less_than[9007199254740993 9007199254740994]", "true"},
      {"greater_than[9007199254740994 9007199254740993]", "true"},
      {"less_than[9007199254740993 9007199254740992.0]", "false"},
      {"greater_than[9007199254740993 9007199254740992.0]", "false"},
      {"less_than[(1 2 3) 2]", "(true false false)"},
      {"greater_than[2 (1 2 3)]", "(true false false)"},
      {"less_than[Int() 1]", "()"},
      {"greater_than[Double() 1]", "()"},
  }};
  for (const OrderingCase &test_case : cases) {
    INFO(std::string(test_case.source));
    CHECK(issue54_evaluate(test_case.source) == std::string(test_case.expected));
  }
}

TEST_CASE("ISSUE54-ERRORS reject Bool ordering conversions arity and shape before work") {
  for (const std::string_view source : {
           "less_than[true false]",
           "greater_than[true 1]",
           "less_than[1 true]",
           "is_positive[true]",
           "odd[1.0]",
       }) {
    INFO(std::string(source));
    const ValueResult result = evaluate_expression(source);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == ErrorKind::type_mismatch);
  }

  const ValueResult arity = evaluate_expression("less_than[1]");
  CHECK_FALSE(arity.ok);
  CHECK(arity.error.kind == ErrorKind::arity_error);

  const ValueResult shape = evaluate_expression("greater_than[(1) (1 2)]");
  CHECK_FALSE(shape.ok);
  CHECK(shape.error.kind == ErrorKind::shape_mismatch);

  const EvaluationConfiguration refused{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::size_t{2U}, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  const ValueResult resource =
      evaluate_expression("less_than[(1 2 3) 4]", refused);
  CHECK_FALSE(resource.ok);
  CHECK(resource.error.kind == ErrorKind::resource_error);
}

TEST_CASE("ISSUE54-RESOURCES preserve shared preflight charging and cleanup semantics") {
  const EvaluationConfiguration no_work{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::size_t{0U}},
      AllocationFailureInjection{std::nullopt}};
  const ValueResult scalar_refused = evaluate_expression("odd[1]", no_work);
  REQUIRE_FALSE(scalar_refused.ok);
  CHECK(scalar_refused.error.kind == ErrorKind::resource_error);
  REQUIRE(scalar_refused.error.resource.has_value());
  REQUIRE(scalar_refused.error.primitive.has_value());
  CHECK(scalar_refused.error.primitive->id == PrimitiveId::odd);
  CHECK(scalar_refused.error.resource->limit_kind ==
        ResourceLimitKind::max_work_units);
  CHECK(scalar_refused.error.resource->usage_before == std::size_t{0U});
  CHECK(scalar_refused.error.resource->refused_charge == std::size_t{1U});

  const EvaluationConfiguration no_resources{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::size_t{0U}, std::size_t{0U}, std::size_t{0U}},
      AllocationFailureInjection{std::nullopt}};
  ValueResult empty =
      evaluate_expression("less_than[Int() 1]", no_resources);
  REQUIRE(empty.ok);
  const ValueFormattingResult empty_formatted = format_value(empty.value);
  REQUIRE(empty_formatted.ok);
  CHECK(empty_formatted.formatted == "()");
  destroy_value(empty.value);

  const EvaluationConfiguration live_refusal{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::size_t{24U}, std::size_t{26U}, std::size_t{3U}},
      AllocationFailureInjection{std::nullopt}};
  const ValueResult live =
      evaluate_expression("less_than[(1 2 3) 4]", live_refusal);
  REQUIRE_FALSE(live.ok);
  REQUIRE(live.error.resource.has_value());
  REQUIRE(live.error.primitive.has_value());
  CHECK(live.error.primitive->id == PrimitiveId::less_than);
  CHECK(live.error.resource->limit_kind ==
        ResourceLimitKind::max_live_evaluation_bytes);
  CHECK(live.error.resource->usage_before == std::size_t{24U});
  CHECK(live.error.resource->refused_charge == std::size_t{3U});
  CHECK(live.error.resource->requested_elements == std::size_t{3U});
  CHECK(live.error.resource->requested_bytes == std::size_t{3U});

  const EvaluationConfiguration allocation_failure{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::size_t{1U}}};
  const ValueResult allocation =
      evaluate_expression("greater_than[(1 2 3) 0]", allocation_failure);
  REQUIRE_FALSE(allocation.ok);
  REQUIRE(allocation.error.resource.has_value());
  REQUIRE(allocation.error.primitive.has_value());
  CHECK(allocation.error.primitive->id == PrimitiveId::greater_than);
  CHECK(allocation.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  CHECK(allocation.error.resource->requested_elements == std::size_t{3U});
  CHECK(allocation.error.resource->requested_bytes == std::size_t{3U});

  const ValueResult shape_before_work =
      evaluate_expression("less_than[(1 2 3) (4 5)]", no_work);
  CHECK_FALSE(shape_before_work.ok);
  CHECK(shape_before_work.error.kind == ErrorKind::shape_mismatch);
}

} // TEST_SUITE

} // namespace bennu
