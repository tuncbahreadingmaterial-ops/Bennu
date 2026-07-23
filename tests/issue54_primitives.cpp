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
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

TEST_CASE("ISSUE54-REGISTRY covers all nine identities and fifteen signatures") {
  struct ExpectedPrimitive {
    PrimitiveId id;
    std::string_view name;
    std::uint8_t stable_id;
    std::size_t signature_count;
  };
  constexpr std::array<ExpectedPrimitive, 9> primitives{{
      {PrimitiveId::logical_and, "and", 5U, 1U},
      {PrimitiveId::logical_or, "or", 6U, 1U},
      {PrimitiveId::not_equals, "not_equals", 7U, 3U},
      {PrimitiveId::odd, "odd", 8U, 1U},
      {PrimitiveId::even, "even", 9U, 1U},
      {PrimitiveId::is_positive, "is_positive", 10U, 2U},
      {PrimitiveId::is_negative, "is_negative", 11U, 2U},
      {PrimitiveId::less_than, "less_than", 12U, 2U},
      {PrimitiveId::greater_than, "greater_than", 13U, 2U},
  }};
  struct ExpectedSignature {
    PrimitiveId id;
    PrimitiveImplementation implementation;
    std::array<ScalarType, 2> parameters;
    std::size_t arity;
  };
  constexpr std::array<ExpectedSignature, 15> signatures{{
      {PrimitiveId::logical_and,
       PrimitiveImplementation::logical_and_boolean,
       {ScalarType::boolean, ScalarType::boolean}, 2U},
      {PrimitiveId::logical_or, PrimitiveImplementation::logical_or_boolean,
       {ScalarType::boolean, ScalarType::boolean}, 2U},
      {PrimitiveId::not_equals, PrimitiveImplementation::not_equals_boolean,
       {ScalarType::boolean, ScalarType::boolean}, 2U},
      {PrimitiveId::not_equals, PrimitiveImplementation::not_equals_integer,
       {ScalarType::integer, ScalarType::integer}, 2U},
      {PrimitiveId::not_equals, PrimitiveImplementation::not_equals_double,
       {ScalarType::double_precision, ScalarType::double_precision}, 2U},
      {PrimitiveId::odd, PrimitiveImplementation::odd_integer,
       {ScalarType::integer, ScalarType::boolean}, 1U},
      {PrimitiveId::even, PrimitiveImplementation::even_integer,
       {ScalarType::integer, ScalarType::boolean}, 1U},
      {PrimitiveId::is_positive,
       PrimitiveImplementation::is_positive_integer,
       {ScalarType::integer, ScalarType::boolean}, 1U},
      {PrimitiveId::is_positive,
       PrimitiveImplementation::is_positive_double,
       {ScalarType::double_precision, ScalarType::boolean}, 1U},
      {PrimitiveId::is_negative,
       PrimitiveImplementation::is_negative_integer,
       {ScalarType::integer, ScalarType::boolean}, 1U},
      {PrimitiveId::is_negative,
       PrimitiveImplementation::is_negative_double,
       {ScalarType::double_precision, ScalarType::boolean}, 1U},
      {PrimitiveId::less_than, PrimitiveImplementation::less_than_integer,
       {ScalarType::integer, ScalarType::integer}, 2U},
      {PrimitiveId::less_than, PrimitiveImplementation::less_than_double,
       {ScalarType::double_precision, ScalarType::double_precision}, 2U},
      {PrimitiveId::greater_than,
       PrimitiveImplementation::greater_than_integer,
       {ScalarType::integer, ScalarType::integer}, 2U},
      {PrimitiveId::greater_than,
       PrimitiveImplementation::greater_than_double,
       {ScalarType::double_precision, ScalarType::double_precision}, 2U},
  }};

  std::size_t issue_signature_count = 0U;
  for (const ExpectedPrimitive &expected : primitives) {
    INFO(std::string(expected.name));
    const PrimitiveDescriptor *by_id = find_primitive(expected.id);
    const PrimitiveDescriptor *by_name = find_primitive(expected.name);
    REQUIRE(by_id != nullptr);
    CHECK(by_name == by_id);
    CHECK(std::string(by_id->name) == std::string(expected.name));
    CHECK(static_cast<std::uint8_t>(by_id->id) == expected.stable_id);
    CHECK(by_id->lifting == LiftingMode::elementwise);
    CHECK(by_id->signature_count == expected.signature_count);
    issue_signature_count += by_id->signature_count;
  }
  CHECK(issue_signature_count == std::size_t{15U});

  for (const ExpectedSignature &expected : signatures) {
    const PrimitiveDescriptor *descriptor = find_primitive(expected.id);
    REQUIRE(descriptor != nullptr);
    const PrimitiveSignature *matched = nullptr;
    for (std::size_t index = 0U; index < descriptor->signature_count; ++index) {
      if (descriptor->signatures[index].implementation ==
          expected.implementation) {
        CHECK(matched == nullptr);
        matched = &descriptor->signatures[index];
      }
    }
    REQUIRE(matched != nullptr);
    CHECK(matched->parameter_count == expected.arity);
    CHECK(matched->result.container == ContainerKind::scalar);
    CHECK(matched->result.element == ScalarType::boolean);
    for (std::size_t index = 0U; index < expected.arity; ++index) {
      CHECK(matched->parameters[index].container == ContainerKind::scalar);
      CHECK(matched->parameters[index].element == expected.parameters[index]);
    }
  }
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

TEST_CASE("ISSUE54-LIFTING covers the complete shared elementwise matrix") {
  struct SuccessCase {
    std::string_view category;
    std::string_view source;
    std::string_view expected;
  };
  constexpr std::array<SuccessCase, 54> cases{{
      // Every implementation signature is exercised with a vector result;
      // every dyadic signature uses vector/vector arguments.
      {"vector-vector", "and[(true false) (true true)]", "(true false)"},
      {"vector-vector", "or[(true false) (false false)]", "(true false)"},
      {"vector-vector", "not_equals[(true false) (false false)]",
       "(true false)"},
      {"vector-vector", "not_equals[(1 2) (1 3)]", "(false true)"},
      {"vector-vector", "not_equals[(1.0 nan) (2.0 nan)]", "(true true)"},
      {"vector", "odd[(1 -2)]", "(true false)"},
      {"vector", "even[(1 -2)]", "(false true)"},
      {"vector", "is_positive[(-1 2)]", "(false true)"},
      {"vector", "is_positive[(-1.0 2.0)]", "(false true)"},
      {"vector", "is_negative[(-1 2)]", "(true false)"},
      {"vector", "is_negative[(-1.0 2.0)]", "(true false)"},
      {"vector-vector", "less_than[(1 3) (2 2)]", "(true false)"},
      {"vector-vector", "less_than[(1.0 nan) (2.0 3.0)]",
       "(true false)"},
      {"vector-vector", "greater_than[(1 3) (2 2)]", "(false true)"},
      {"vector-vector", "greater_than[(1.0 nan) (2.0 3.0)]",
       "(false false)"},

      // A vector in every dyadic argument position proves scalar broadcasting.
      {"scalar-vector", "and[true (true false)]", "(true false)"},
      {"vector-scalar", "and[(true false) true]", "(true false)"},
      {"scalar-vector", "or[false (true false)]", "(true false)"},
      {"vector-scalar", "or[(true false) false]", "(true false)"},
      {"scalar-vector", "not_equals[1 (1 2)]", "(false true)"},
      {"vector-scalar", "not_equals[(1 2) 1]", "(false true)"},
      {"scalar-vector", "less_than[2 (1 3)]", "(false true)"},
      {"vector-scalar", "less_than[(1 3) 2]", "(true false)"},
      {"scalar-vector", "greater_than[2 (1 3)]", "(true false)"},
      {"vector-scalar", "greater_than[(1 3) 2]", "(false true)"},

      // Singleton vectors retain rank and never broadcast as scalars.
      {"singleton", "and[(true) true]", "(true)"},
      {"singleton", "or[(false) false]", "(false)"},
      {"singleton", "not_equals[(1) 2]", "(true)"},
      {"singleton", "odd[(1)]", "(true)"},
      {"singleton", "even[(2)]", "(true)"},
      {"singleton", "is_positive[(1)]", "(true)"},
      {"singleton", "is_negative[(-1)]", "(true)"},
      {"singleton", "less_than[(1) 2]", "(true)"},
      {"singleton", "greater_than[(2) 1]", "(true)"},

      // Typed empties preserve the selected Bool result type and charge no work.
      {"empty-scalar", "and[Bool() true]", "()"},
      {"empty-scalar", "or[false Bool()]", "()"},
      {"empty-scalar", "not_equals[Int() 1]", "()"},
      {"empty-scalar", "less_than[1 Double()]", "()"},
      {"empty-scalar", "greater_than[Int() 1]", "()"},
      {"empty-pair", "and[Bool() Bool()]", "()"},
      {"empty-pair", "or[Bool() Bool()]", "()"},
      {"empty-pair", "not_equals[Int() Int()]", "()"},
      {"empty-scalar", "odd[Int()]", "()"},
      {"empty-scalar", "even[Int()]", "()"},
      {"empty-scalar", "is_positive[Double()]", "()"},
      {"empty-scalar", "is_negative[Int()]", "()"},
      {"empty-pair", "less_than[Double() Double()]", "()"},
      {"empty-pair", "greater_than[Int() Int()]", "()"},

      // Mixed numeric empties and vectors select the Double implementation.
      {"mixed-empty", "not_equals[Int() Double()]", "()"},
      {"mixed-empty", "less_than[Double() Int()]", "()"},
      {"mixed-empty", "greater_than[Int() Double()]", "()"},
      {"promotion-vector",
       "less_than[(9007199254740993) 9007199254740992.0]", "(false)"},
      {"promotion-vector",
       "greater_than[9007199254740994.0 (9007199254740993)]", "(true)"},
      {"pointwise", "not_equals[(1 2 3) (1 0 3)]",
       "(false true false)"},
  }};

  for (const SuccessCase &test_case : cases) {
    INFO(std::string(test_case.category));
    INFO(std::string(test_case.source));
    CHECK(issue54_evaluate(test_case.source) == std::string(test_case.expected));
  }

  struct SelectionCase {
    PrimitiveId id;
    std::array<ScalarType, 2> actual;
    std::size_t expected_index;
    std::size_t expected_cost;
  };
  constexpr std::array<SelectionCase, 9> selections{{
      {PrimitiveId::not_equals,
       {ScalarType::integer, ScalarType::integer}, 1U, 0U},
      {PrimitiveId::not_equals,
       {ScalarType::integer, ScalarType::double_precision}, 2U, 1U},
      {PrimitiveId::not_equals,
       {ScalarType::double_precision, ScalarType::integer}, 2U, 1U},
      {PrimitiveId::less_than, {ScalarType::integer, ScalarType::integer}, 0U,
       0U},
      {PrimitiveId::less_than,
       {ScalarType::integer, ScalarType::double_precision}, 1U, 1U},
      {PrimitiveId::less_than,
       {ScalarType::double_precision, ScalarType::integer}, 1U, 1U},
      {PrimitiveId::greater_than,
       {ScalarType::integer, ScalarType::integer}, 0U, 0U},
      {PrimitiveId::greater_than,
       {ScalarType::integer, ScalarType::double_precision}, 1U, 1U},
      {PrimitiveId::greater_than,
       {ScalarType::double_precision, ScalarType::integer}, 1U, 1U},
  }};
  for (const SelectionCase &test_case : selections) {
    const PrimitiveDescriptor *descriptor = find_primitive(test_case.id);
    REQUIRE(descriptor != nullptr);
    const SignatureSelectionResult selected =
        select_primitive_signature(*descriptor, test_case.actual);
    REQUIRE(selected.status == SignatureSelectionStatus::success);
    CHECK(selected.signature_index == test_case.expected_index);
    CHECK(selected.promotion_cost == test_case.expected_cost);
  }
}

TEST_CASE("ISSUE54-VALIDATION covers arity type shape and precedence matrix") {
  struct ErrorCase {
    std::string_view category;
    std::string_view source;
    ErrorKind kind;
  };
  // Shape-before-domain, resource-before-domain, and lowest-domain-index are
  // explicitly not applicable: Issue #54 kernels cannot produce DomainError.
  // Every failure below asserts that no domain context was populated.
  constexpr std::array<ErrorCase, 33> cases{{
      {"arity", "and[true]", ErrorKind::arity_error},
      {"arity", "or[false true false]", ErrorKind::arity_error},
      {"arity", "not_equals[1]", ErrorKind::arity_error},
      {"arity", "odd[1 2]", ErrorKind::arity_error},
      {"arity", "even[]", ErrorKind::arity_error},
      {"arity", "is_positive[1 2]", ErrorKind::arity_error},
      {"arity", "is_negative[]", ErrorKind::arity_error},
      {"arity", "less_than[1]", ErrorKind::arity_error},
      {"arity", "greater_than[1 2 3]", ErrorKind::arity_error},
      {"type", "and[true 1]", ErrorKind::type_mismatch},
      {"type", "or[0 false]", ErrorKind::type_mismatch},
      {"type", "not_equals[true 1]", ErrorKind::type_mismatch},
      {"type", "odd[1.0]", ErrorKind::type_mismatch},
      {"type", "even[false]", ErrorKind::type_mismatch},
      {"type", "is_positive[true]", ErrorKind::type_mismatch},
      {"type", "is_negative[Bool()]", ErrorKind::type_mismatch},
      {"type", "less_than[true false]", ErrorKind::type_mismatch},
      {"type", "greater_than[1 true]", ErrorKind::type_mismatch},
      {"shape", "and[(true false) (true)]", ErrorKind::shape_mismatch},
      {"shape", "or[(true) (false true)]", ErrorKind::shape_mismatch},
      {"shape", "not_equals[(1 2) (1)]", ErrorKind::shape_mismatch},
      {"shape", "less_than[(1) (1 2)]", ErrorKind::shape_mismatch},
      {"shape", "greater_than[(1 2) (1)]", ErrorKind::shape_mismatch},
      {"empty-nonempty", "and[Bool() (true)]", ErrorKind::shape_mismatch},
      {"empty-nonempty", "or[(false) Bool()]", ErrorKind::shape_mismatch},
      {"empty-nonempty", "not_equals[Int() (1)]", ErrorKind::shape_mismatch},
      {"empty-nonempty", "less_than[(1) Int()]", ErrorKind::shape_mismatch},
      {"empty-nonempty", "greater_than[Double() (1.0)]",
       ErrorKind::shape_mismatch},
      {"type-before-shape", "and[(true false) (1 2 3)]",
       ErrorKind::type_mismatch},
      {"type-before-shape", "or[(true false) (1 2 3)]",
       ErrorKind::type_mismatch},
      {"type-before-shape", "not_equals[(true false) (1 2 3)]",
       ErrorKind::type_mismatch},
      {"type-before-shape", "less_than[(true false) (1 2 3)]",
       ErrorKind::type_mismatch},
      {"type-before-shape", "greater_than[(1 2) (true false true)]",
       ErrorKind::type_mismatch},
  }};
  for (const ErrorCase &test_case : cases) {
    INFO(std::string(test_case.category));
    INFO(std::string(test_case.source));
    const ValueResult result = evaluate_expression(test_case.source);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == test_case.kind);
    CHECK_FALSE(result.error.domain.has_value());
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

TEST_CASE("ISSUE54-WORK-CHARGE observes kernels work and transactional results") {
  const PrimitiveDescriptor *less_than = find_primitive(PrimitiveId::less_than);
  REQUIRE(less_than != nullptr);

  ValueResult left = evaluate_expression("(1 2 3)");
  ValueResult right = evaluate_expression("(2 2 2)");
  REQUIRE(left.ok);
  REQUIRE(right.ok);
  std::array<Value, 2> arguments{{std::move(left.value),
                                  std::move(right.value)}};

  EvaluationResources exact = make_bounded_resources(
      ResourceLimits{std::size_t{3U}, std::size_t{3U}, std::size_t{3U}},
      AllocationFailureInjection{std::nullopt});
  PrimitiveApplicationContext exact_context{exact, 0U};
  PrimitiveApplicationResult success = apply_primitive(
      exact_context, *less_than, arguments, SourceLocation{1U, 1U, 1U});
  REQUIRE(success.ok);
  CHECK(success.value.container == ContainerKind::vector);
  CHECK(success.value.vector.element_type == ScalarType::boolean);
  CHECK(issue54_evaluate("less_than[(1 2 3) (2 2 2)]") ==
        "(true false false)");
  CHECK(exact_context.scalar_kernel_invocations == std::size_t{3U});
  CHECK(exact.work_units == std::size_t{3U});
  CHECK(exact.live_evaluation_bytes == std::size_t{3U});
  release_vector_reservation(exact, success.value);
  CHECK(exact.live_evaluation_bytes == std::size_t{0U});

  EvaluationResources refused = make_bounded_resources(
      ResourceLimits{std::size_t{3U}, std::size_t{3U}, std::size_t{2U}},
      AllocationFailureInjection{std::nullopt});
  PrimitiveApplicationContext refused_context{refused, 0U};
  const PrimitiveApplicationResult failure = apply_primitive(
      refused_context, *less_than, arguments, SourceLocation{1U, 1U, 1U});
  REQUIRE_FALSE(failure.ok);
  CHECK(failure.error.kind == ErrorKind::resource_error);
  REQUIRE(failure.error.resource.has_value());
  CHECK(failure.error.resource->limit_kind ==
        ResourceLimitKind::max_work_units);
  CHECK(failure.error.resource->usage_before == std::size_t{0U});
  CHECK(failure.error.resource->refused_charge == std::size_t{3U});
  CHECK(failure.error.resource->requested_elements == std::size_t{3U});
  CHECK(failure.error.resource->requested_bytes == std::size_t{3U});
  CHECK(refused_context.scalar_kernel_invocations == std::size_t{0U});
  CHECK(refused.work_units == std::size_t{0U});
  CHECK(refused.live_evaluation_bytes == std::size_t{0U});
  CHECK(failure.value.container == ContainerKind::scalar);

  destroy_value(arguments[0]);
  destroy_value(arguments[1]);

  ValueResult empty_left = evaluate_expression("Int()");
  ValueResult empty_right = evaluate_expression("Double()");
  REQUIRE(empty_left.ok);
  REQUIRE(empty_right.ok);
  std::array<Value, 2> empty_arguments{{std::move(empty_left.value),
                                        std::move(empty_right.value)}};
  EvaluationResources empty_resources = make_bounded_resources(
      ResourceLimits{std::size_t{0U}, std::size_t{0U}, std::size_t{0U}},
      AllocationFailureInjection{std::nullopt});
  PrimitiveApplicationContext empty_context{empty_resources, 0U};
  PrimitiveApplicationResult empty_result = apply_primitive(
      empty_context, *less_than, empty_arguments, SourceLocation{1U, 1U, 1U});
  REQUIRE(empty_result.ok);
  CHECK(empty_result.value.container == ContainerKind::vector);
  CHECK(empty_result.value.vector.element_type == ScalarType::boolean);
  CHECK(empty_context.scalar_kernel_invocations == std::size_t{0U});
  CHECK(empty_resources.work_units == std::size_t{0U});
  CHECK(empty_resources.live_evaluation_bytes == std::size_t{0U});
  destroy_value(empty_result.value);
  destroy_value(empty_arguments[0]);
  destroy_value(empty_arguments[1]);
}

} // TEST_SUITE

} // namespace bennu
