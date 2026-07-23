#include "bennu/application.hpp"
#include "bennu/resources.hpp"
#include "bennu/type.hpp"
#include "bennu/value.hpp"

#include "doctest/doctest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <utility>

namespace bennu {

namespace {

constexpr SourceLocation tuple_location{11U, 2U, 4U};
constexpr AllocationFailureInjection no_failure{std::nullopt};

ResourceLimits v2_limits(std::optional<std::size_t> vector_bytes,
                         std::optional<std::size_t> live_bytes,
                         std::optional<std::size_t> work_units,
                         std::optional<std::size_t> tuple_bytes) {
  return ResourceLimits{vector_bytes, live_bytes, work_units, tuple_bytes};
}

Value make_test_vector(EvaluationResources &resources,
                       std::initializer_list<std::int64_t> elements) {
  VectorAllocationResult allocated = copy_int_vector(
      resources,
      std::span<const std::int64_t>(elements.begin(), elements.size()),
      tuple_location, "tuple-test-vector");
  REQUIRE(allocated.ok);
  return move_value(allocated.value);
}

std::string valid_type_text(const TypeArena &type) {
  const TypeFormattingResult formatted = format_type(type);
  REQUIRE(formatted.ok);
  return formatted.formatted;
}

std::string valid_value_text(const Value &value) {
  const ValueFormattingResult formatted = format_value(value);
  REQUIRE(formatted.ok);
  return formatted.formatted;
}

} // namespace

TEST_CASE("TUP-002-TYPES") {
  const TypeArena integer = make_scalar_type(ScalarType::integer);
  const TypeArena vector_boolean = make_vector_type(ScalarType::boolean);
  const TypeConstructionResult empty = make_tuple_type({});
  REQUIRE(empty.ok);
  CHECK(valid_type_text(empty.type) == "Tuple<>");

  const std::array<TypeArena, 3> element_types{{
      integer,
      make_scalar_type(ScalarType::double_precision),
      vector_boolean,
  }};
  const TypeConstructionResult heterogeneous = make_tuple_type(element_types);
  REQUIRE(heterogeneous.ok);
  CHECK(valid_type_text(heterogeneous.type) ==
        "Tuple<Int, Double, Vector<Bool>>");
  CHECK(validate_type(heterogeneous.type).ok);
  CHECK(heterogeneous.type.root_index == heterogeneous.type.nodes.size() - 1U);

  TypeArena copied_diagnostic_type = heterogeneous.type;
  CHECK(structural_type_equal(copied_diagnostic_type, heterogeneous.type));
  CHECK(valid_type_text(copied_diagnostic_type) ==
        "Tuple<Int, Double, Vector<Bool>>");

  TypeArena deep = make_scalar_type(ScalarType::integer);
  constexpr std::size_t depth = 4096U;
  for (std::size_t index = 0U; index < depth; ++index) {
    const std::array<TypeArena, 1> child{{std::move(deep)}};
    TypeConstructionResult wrapped = make_tuple_type(child);
    REQUIRE(wrapped.ok);
    deep = std::move(wrapped.type);
  }
  const TypeFormattingResult deep_text = format_type(deep);
  REQUIRE(deep_text.ok);
  CHECK(deep_text.formatted.size() ==
        depth * std::string("Tuple<>").size() + std::string("Int").size());
  CHECK(deep_text.formatted.starts_with("Tuple<Tuple<Tuple<"));
  CHECK(deep_text.formatted.ends_with(">>>"));

  TypeArena invalid = heterogeneous.type;
  invalid.nodes.back().first_child = invalid.child_indexes.size() + 1U;
  const TypeValidationResult invalid_result = validate_type(invalid);
  CHECK_FALSE(invalid_result.ok);
  CHECK(invalid_result.invariant == TypeInvariant::invalid_tuple_range);
  CHECK_FALSE(format_type(invalid).ok);

  const auto check_invalid = [](const TypeArena &candidate,
                                TypeInvariant expected) {
    const TypeValidationResult result = validate_type(candidate);
    CHECK_FALSE(result.ok);
    CHECK(result.invariant == expected);
  };
  TypeArena invalid_root = heterogeneous.type;
  invalid_root.root_index = invalid_root.nodes.size();
  check_invalid(invalid_root, TypeInvariant::invalid_type_root);
  TypeArena nonfinal_root = heterogeneous.type;
  nonfinal_root.root_index = 0U;
  check_invalid(nonfinal_root, TypeInvariant::nonfinal_type_root);
  TypeArena unknown_kind = heterogeneous.type;
  unknown_kind.nodes[0].kind = static_cast<TypeKind>(99);
  check_invalid(unknown_kind, TypeInvariant::unknown_type_kind);
  TypeArena unknown_scalar = heterogeneous.type;
  unknown_scalar.nodes[0].scalar = static_cast<ScalarType>(99);
  check_invalid(unknown_scalar, TypeInvariant::unknown_scalar_type);
  TypeArena inactive_field = heterogeneous.type;
  inactive_field.nodes[0].child_count = 1U;
  check_invalid(inactive_field, TypeInvariant::inactive_type_field);
  TypeArena invalid_child = heterogeneous.type;
  invalid_child.child_indexes[0] = invalid_child.nodes.size();
  check_invalid(invalid_child, TypeInvariant::invalid_tuple_child_index);
  TypeArena non_postorder = heterogeneous.type;
  non_postorder.child_indexes[0] = non_postorder.root_index;
  check_invalid(non_postorder, TypeInvariant::non_postorder_tuple_child);
  TypeArena aliased_child = heterogeneous.type;
  aliased_child.child_indexes[1] = aliased_child.child_indexes[0];
  check_invalid(aliased_child, TypeInvariant::aliased_tuple_child);
  TypeArena orphan_edge = heterogeneous.type;
  orphan_edge.child_indexes.push_back(0U);
  check_invalid(orphan_edge, TypeInvariant::orphan_tuple_edge);
  TypeArena orphan_node = heterogeneous.type;
  orphan_node.nodes.insert(
      orphan_node.nodes.end() - 1,
      TypeNode{TypeKind::scalar, ScalarType::boolean, 0U, 0U});
  ++orphan_node.root_index;
  check_invalid(orphan_node, TypeInvariant::orphan_type_node);

  const std::array<TypeArena, 1> nested_child{{integer}};
  const TypeConstructionResult nested_tuple = make_tuple_type(nested_child);
  REQUIRE(nested_tuple.ok);
  const std::array<TypeArena, 2> nested_elements{{nested_tuple.type, integer}};
  const TypeConstructionResult nested_owner = make_tuple_type(nested_elements);
  REQUIRE(nested_owner.ok);
  TypeArena overlapping_range = nested_owner.type;
  overlapping_range.nodes[1].first_child =
      overlapping_range.nodes.back().first_child;
  check_invalid(overlapping_range, TypeInvariant::overlapping_tuple_range);
}

TEST_CASE("TUP-003-VALUES") {
  EvaluationResources detached_resources =
      make_trusted_local_v2_resources(no_failure);
  VectorAllocationResult detached_vector = allocate_vector(
      detached_resources, ScalarType::integer, 2U, 0U, tuple_location,
      "detached-vector");
  REQUIRE(detached_vector.ok);
  detached_vector.value.vector.accounting_active = false;
  detached_vector.value.vector.accounting_owner = nullptr;
  CHECK(validate_value(detached_vector.value).ok);
  CHECK(destroy_value(detached_vector.value).ok);

  Value conflicting_scalar = make_int_value(4);
  conflicting_scalar.vector.element_type = ScalarType::integer;
  conflicting_scalar.tuple.first_child = 1U;
  const ValueValidationResult conflicting_scalar_result =
      validate_value(conflicting_scalar);
  CHECK_FALSE(conflicting_scalar_result.ok);
  CHECK(conflicting_scalar_result.invariant ==
        ValueInvariant::inactive_vector_payload);
  CHECK(conflicting_scalar_result.path.empty());
  CHECK(conflicting_scalar_result.node_index ==
        std::optional<std::size_t>{0U});

  EvaluationResources resources = make_trusted_local_v2_resources(no_failure);
  std::array<Value, 3> children{{make_int_value(1), make_double_value(2.5),
                                make_bool_value(true)}};
  TupleConstructionResult made = make_tuple_value(
      resources, children, tuple_location, "tuple-construction");
  REQUIRE(made.ok);
  CHECK(valid_value_text(made.value) == "[1 2.5 true]");
  CHECK(validate_value(made.value).ok);
  CHECK_FALSE(children[0].claimed);
  CHECK_FALSE(children[1].claimed);
  CHECK_FALSE(children[2].claimed);

  const ValueTypeResult type = value_type(made.value);
  REQUIRE(type.ok);
  CHECK(valid_type_text(type.type) == "Tuple<Int, Double, Bool>");

  const ValueTupleArityResult arity = value_tuple_arity(made.value);
  REQUIRE(arity.ok);
  CHECK(arity.arity == 3U);
  const ValueTupleElementResult element = value_tuple_element(made.value, 1U);
  REQUIRE(element.ok);
  const ValueTypeResult element_type = value_type(element.view);
  REQUIRE(element_type.ok);
  CHECK(valid_type_text(element_type.type) == "Double");
  CHECK_FALSE(value_tuple_element(made.value, 3U).ok);

  ScalarType scalar_type = ScalarType::double_precision;
  const ValueValidationResult legacy_element =
      value_element_type(made.value, scalar_type);
  CHECK_FALSE(legacy_element.ok);
  CHECK(legacy_element.error == ValueAccessError::container_mismatch);
  std::size_t legacy_length = 99U;
  const ValueValidationResult legacy_length_result =
      value_length(made.value, legacy_length);
  CHECK_FALSE(legacy_length_result.ok);
  CHECK(legacy_length_result.error == ValueAccessError::container_mismatch);
  CHECK(legacy_length == 0U);

  Value bad_root = move_value(made.value);
  bad_root.tuple.root_index = bad_root.tuple.nodes.size() + 1U;
  const ValueValidationResult bad_root_result = validate_value(bad_root);
  CHECK_FALSE(bad_root_result.ok);
  CHECK(bad_root_result.invariant == ValueInvariant::invalid_value_root);
  CHECK_FALSE(format_value(bad_root).ok);

  const void *reservation_before = bad_root.tuple.root_reservation.storage.get();
  const ValueDestructionResult refused_destroy = destroy_value(bad_root);
  CHECK_FALSE(refused_destroy.ok);
  CHECK(bad_root.tuple.root_reservation.storage.get() == reservation_before);
  bad_root.tuple.root_index = bad_root.tuple.nodes.size();
  CHECK(destroy_value(bad_root).ok);
  CHECK_FALSE(bad_root.claimed);

  std::array<Value, 1> nested_leaf{{make_int_value(9)}};
  TupleConstructionResult nested = make_tuple_value(
      resources, nested_leaf, tuple_location, "nested-path");
  REQUIRE(nested.ok);
  std::array<Value, 1> nested_owner{{move_value(nested.value)}};
  TupleConstructionResult nested_outer = make_tuple_value(
      resources, nested_owner, tuple_location, "nested-path");
  REQUIRE(nested_outer.ok);
  nested_outer.value.tuple.nodes[0].first_child = 1U;
  const ValueValidationResult nested_invalid =
      validate_value(nested_outer.value);
  CHECK_FALSE(nested_invalid.ok);
  CHECK(nested_invalid.path == std::vector<std::size_t>{0U, 0U});
  const ValueFormattingResult invalid_format =
      format_value(nested_outer.value);
  CHECK_FALSE(invalid_format.ok);
  CHECK(invalid_format.path == std::vector<std::size_t>{0U, 0U});
  const ValueTupleArityResult invalid_arity =
      value_tuple_arity(nested_outer.value);
  CHECK_FALSE(invalid_arity.ok);
  CHECK(invalid_arity.path == std::vector<std::size_t>{0U, 0U});
  const ValueDestructionResult invalid_destruction =
      destroy_value(nested_outer.value);
  CHECK_FALSE(invalid_destruction.ok);
  CHECK(invalid_destruction.path == std::vector<std::size_t>{0U, 0U});
  CHECK(nested_outer.value.tuple.nodes[0].first_child == 1U);
  nested_outer.value.tuple.nodes[0].first_child = 0U;
  CHECK(release_value_reservations(resources, nested_outer.value).ok);

  std::array<Value, 2> vector_children{{
      make_test_vector(resources, {1}),
      make_test_vector(resources, {2}),
  }};
  TupleConstructionResult vector_tuple = make_tuple_value(
      resources, vector_children, tuple_location, "payload-handles");
  REQUIRE(vector_tuple.ok);
  vector_tuple.value.tuple.nodes[1].vector_payload_index =
      vector_tuple.value.tuple.nodes[0].vector_payload_index;
  CHECK(validate_value(vector_tuple.value).invariant ==
        ValueInvariant::aliased_vector_payload);
  vector_tuple.value.tuple.nodes[1].vector_payload_index = 1U;
  VectorAllocationResult empty_payload = allocate_vector(
      resources, ScalarType::integer, 0U, 0U, tuple_location,
      "orphan-payload");
  REQUIRE(empty_payload.ok);
  vector_tuple.value.tuple.vector_payloads.push_back(
      std::move(empty_payload.value.vector));
  CHECK(validate_value(vector_tuple.value).invariant ==
        ValueInvariant::orphan_vector_payload_handle);
  vector_tuple.value.tuple.vector_payloads.pop_back();
  CHECK(release_value_reservations(resources, vector_tuple.value).ok);

  EvaluationResources invariant_resources =
      make_trusted_local_v2_resources(no_failure);
  const auto make_pair = [&invariant_resources]() {
    std::array<Value, 2> pair{{make_int_value(1), make_bool_value(false)}};
    TupleConstructionResult result = make_tuple_value(
        invariant_resources, pair, tuple_location, "invalid-value-probe");
    REQUIRE(result.ok);
    return move_value(result.value);
  };
  const auto check_invalid = [](const Value &candidate,
                                ValueInvariant expected) {
    const ValueValidationResult result = validate_value(candidate);
    CHECK_FALSE(result.ok);
    CHECK(result.invariant == expected);
  };

  Value nonfinal_root = make_pair();
  nonfinal_root.tuple.root_index = 0U;
  check_invalid(nonfinal_root, ValueInvariant::nonfinal_value_root);
  Value unknown_container = make_pair();
  unknown_container.tuple.nodes[0].container =
      static_cast<ContainerKind>(99);
  check_invalid(unknown_container, ValueInvariant::unknown_container);
  Value unknown_scalar = make_pair();
  unknown_scalar.tuple.nodes[0].scalar.type = static_cast<ScalarType>(99);
  check_invalid(unknown_scalar, ValueInvariant::unknown_scalar_type);
  Value inactive_scalar_node = make_pair();
  inactive_scalar_node.tuple.nodes[0].first_child = 1U;
  check_invalid(inactive_scalar_node, ValueInvariant::inactive_tuple_field);
  Value inactive_tuple_scalar = make_pair();
  inactive_tuple_scalar.scalar.integer = 1;
  check_invalid(inactive_tuple_scalar, ValueInvariant::inactive_scalar_field);
  Value inactive_tuple_vector = make_pair();
  inactive_tuple_vector.vector.element_type = ScalarType::integer;
  check_invalid(inactive_tuple_vector, ValueInvariant::inactive_vector_payload);
  Value invalid_root_wins_before_inactive_fields = make_pair();
  invalid_root_wins_before_inactive_fields.tuple.root_index =
      invalid_root_wins_before_inactive_fields.tuple.nodes.size() + 1U;
  invalid_root_wins_before_inactive_fields.scalar.integer = 1;
  check_invalid(invalid_root_wins_before_inactive_fields,
                ValueInvariant::invalid_value_root);
  HostAllocationFailureInjection root_winner_allocation_failure{0U, 0U};
  const ValueValidationResult root_winner_under_pressure = validate_value(
      invalid_root_wins_before_inactive_fields,
      root_winner_allocation_failure);
  CHECK_FALSE(root_winner_under_pressure.ok);
  CHECK(root_winner_under_pressure.invariant ==
        ValueInvariant::invalid_value_root);
  CHECK(root_winner_under_pressure.resource_error ==
        HostResourceErrorReason::none);
  Value invalid_range = make_pair();
  invalid_range.tuple.first_child = invalid_range.tuple.child_indexes.size() + 1U;
  check_invalid(invalid_range, ValueInvariant::invalid_tuple_range);
  Value invalid_child_index = make_pair();
  invalid_child_index.tuple.child_indexes[0] =
      invalid_child_index.tuple.root_index + 1U;
  check_invalid(invalid_child_index,
                ValueInvariant::invalid_tuple_child_index);
  Value non_postorder = make_pair();
  non_postorder.tuple.child_indexes[0] = non_postorder.tuple.root_index;
  check_invalid(non_postorder, ValueInvariant::non_postorder_tuple_child);
  Value aliased_child = make_pair();
  aliased_child.tuple.child_indexes[1] =
      aliased_child.tuple.child_indexes[0];
  check_invalid(aliased_child, ValueInvariant::aliased_tuple_child);
  Value invalid_reservation_count = make_pair();
  ++invalid_reservation_count.tuple.root_reservation.element_count;
  check_invalid(invalid_reservation_count,
                ValueInvariant::invalid_tuple_reservation_count);
  Value orphan_edge = make_pair();
  orphan_edge.tuple.child_indexes.push_back(0U);
  check_invalid(orphan_edge, ValueInvariant::orphan_tuple_edge);
  Value orphan_node = make_pair();
  orphan_node.tuple.nodes.push_back(ValueNode{
      ContainerKind::scalar,
      ScalarValue{ScalarType::integer, false, 7, 0.0}, 0U, 0U, 0U, 0U});
  ++orphan_node.tuple.root_index;
  check_invalid(orphan_node, ValueInvariant::orphan_value_node);
  Value orphan_payload = make_pair();
  VectorAllocationResult detached_empty_vector = allocate_vector(
      invariant_resources, ScalarType::integer, 0U, 0U, tuple_location,
      "orphan-vector-payload");
  REQUIRE(detached_empty_vector.ok);
  orphan_payload.tuple.vector_payloads.push_back(
      std::move(detached_empty_vector.value.vector));
  check_invalid(orphan_payload, ValueInvariant::orphan_vector_payload_handle);
  Value orphan_reservation = make_pair();
  orphan_reservation.tuple.reservations.push_back(TupleTableReservation{});
  check_invalid(orphan_reservation, ValueInvariant::orphan_tuple_reservation);

  const auto make_nested = [&invariant_resources]() {
    std::array<Value, 1> inner_child{{make_int_value(1)}};
    TupleConstructionResult inner = make_tuple_value(
        invariant_resources, inner_child, tuple_location, "nested-invariant");
    REQUIRE(inner.ok);
    std::array<Value, 1> outer_child{{move_value(inner.value)}};
    TupleConstructionResult outer = make_tuple_value(
        invariant_resources, outer_child, tuple_location, "nested-invariant");
    REQUIRE(outer.ok);
    return move_value(outer.value);
  };
  Value overlapping_range = make_nested();
  overlapping_range.tuple.nodes[1].first_child =
      overlapping_range.tuple.first_child;
  check_invalid(overlapping_range, ValueInvariant::overlapping_tuple_range);
  Value missing_reservation = make_nested();
  missing_reservation.tuple.nodes[1].tuple_reservation_index =
      missing_reservation.tuple.reservations.size() + 1U;
  check_invalid(missing_reservation,
                ValueInvariant::missing_tuple_reservation);
  Value aliased_reservation = make_nested();
  aliased_reservation.tuple.nodes[1].tuple_reservation_index =
      aliased_reservation.tuple.reservations.size();
  check_invalid(aliased_reservation,
                ValueInvariant::aliased_tuple_reservation);

  Value invalid_payload_handle = make_pair();
  invalid_payload_handle.tuple.nodes[0].container = ContainerKind::vector;
  invalid_payload_handle.tuple.nodes[0].scalar =
      ScalarValue{ScalarType::boolean, false, 0, 0.0};
  invalid_payload_handle.tuple.nodes[0].vector_payload_index = 1U;
  check_invalid(invalid_payload_handle,
                ValueInvariant::invalid_vector_payload_handle);

  std::array<Value, 2> distinct_vector_children{{
      make_test_vector(invariant_resources, {1}),
      make_test_vector(invariant_resources, {2}),
  }};
  TupleConstructionResult duplicate_pointer_tuple = make_tuple_value(
      invariant_resources, distinct_vector_children, tuple_location,
      "duplicate-payload-pointer");
  REQUIRE(duplicate_pointer_tuple.ok);
  VectorValue &first_payload =
      duplicate_pointer_tuple.value.tuple.vector_payloads[0];
  VectorValue &second_payload =
      duplicate_pointer_tuple.value.tuple.vector_payloads[1];
  std::int64_t *second_original = second_payload.integers.release();
  second_payload.integers.reset(first_payload.integers.get());
  check_invalid(duplicate_pointer_tuple.value,
                ValueInvariant::aliased_vector_payload);
  (void)second_payload.integers.release();
  second_payload.integers.reset(second_original);
  CHECK(release_value_reservations(invariant_resources,
                                   duplicate_pointer_tuple.value)
            .ok);

  Value duplicate_reservation = make_nested();
  TupleTableReservation &nested_reservation =
      duplicate_reservation.tuple.reservations[0];
  std::byte *nested_storage = nested_reservation.storage.release();
  nested_reservation.storage.reset(
      duplicate_reservation.tuple.root_reservation.storage.get());
  check_invalid(duplicate_reservation,
                ValueInvariant::aliased_tuple_reservation);
  (void)nested_reservation.storage.release();
  nested_reservation.storage.reset(nested_storage);
  CHECK(release_value_reservations(invariant_resources,
                                   duplicate_reservation)
            .ok);

  std::array<Value, 1> cross_kind_child{{
      make_test_vector(invariant_resources, {1}),
  }};
  TupleConstructionResult cross_kind = make_tuple_value(
      invariant_resources, cross_kind_child, tuple_location,
      "cross-kind-payload-pointer");
  REQUIRE(cross_kind.ok);
  VectorValue &cross_kind_payload =
      cross_kind.value.tuple.vector_payloads[0];
  std::int64_t *cross_kind_storage = cross_kind_payload.integers.release();
  cross_kind_payload.integers.reset(reinterpret_cast<std::int64_t *>(
      cross_kind.value.tuple.root_reservation.storage.get()));
  check_invalid(cross_kind.value, ValueInvariant::aliased_vector_payload);
  (void)cross_kind_payload.integers.release();
  cross_kind_payload.integers.reset(cross_kind_storage);
  CHECK(release_value_reservations(invariant_resources, cross_kind.value).ok);
}

TEST_CASE("TUP-004-MOVE-CLEANUP") {
  EvaluationResources resources = make_bounded_v2_resources(
      v2_limits(16U, 64U, std::nullopt, 32U), no_failure);

  Value vector = make_test_vector(resources, {1, 2});
  std::array<Value, 1> inner_child{{make_bool_value(true)}};
  TupleConstructionResult inner = make_tuple_value(
      resources, inner_child, tuple_location, "inner-tuple");
  REQUIRE(inner.ok);
  CHECK(resources.live_evaluation_bytes == 32U);

  std::array<Value, 2> outer_children{{move_value(vector),
                                      move_value(inner.value)}};
  TupleConstructionResult outer = make_tuple_value(
      resources, outer_children, tuple_location, "outer-tuple");
  REQUIRE(outer.ok);
  CHECK(resources.live_evaluation_bytes == 64U);
  CHECK(resources.work_units == 0U);
  CHECK(resources.reservation_ordinal == 3U);
  CHECK(valid_value_text(outer.value) == "[(1 2) [true]]");
  CHECK_FALSE(vector.claimed);
  CHECK_FALSE(inner.value.claimed);
  CHECK_FALSE(outer_children[0].claimed);
  CHECK_FALSE(outer_children[1].claimed);

  const ValueReleaseResult released =
      release_value_reservations(resources, outer.value);
  REQUIRE(released.ok);
  CHECK(resources.live_evaluation_bytes == 0U);
  CHECK(resources.work_units == 0U);
  CHECK_FALSE(outer.value.claimed);
  CHECK(destroy_value(outer.value).ok);
  CHECK(resources.live_evaluation_bytes == 0U);

  EvaluationResources destroy_resources =
      make_trusted_local_v2_resources(no_failure);
  std::array<Value, 1> destroy_child{{make_int_value(1)}};
  TupleConstructionResult destroy_tuple = make_tuple_value(
      destroy_resources, destroy_child, tuple_location,
      "active-destroy-accounting");
  REQUIRE(destroy_tuple.ok);
  CHECK(destroy_resources.live_evaluation_bytes == 16U);
  CHECK(destroy_value(destroy_tuple.value).ok);
  CHECK(destroy_resources.live_evaluation_bytes == 0U);

  VectorAllocationResult direct_vector = allocate_vector(
      destroy_resources, ScalarType::integer, 1U, 0U, tuple_location,
      "active-vector-destroy-accounting");
  REQUIRE(direct_vector.ok);
  CHECK(destroy_resources.live_evaluation_bytes == sizeof(std::int64_t));
  CHECK(destroy_value(direct_vector.value).ok);
  CHECK(destroy_resources.live_evaluation_bytes == 0U);

  EvaluationResources first_context =
      make_trusted_local_v2_resources(no_failure);
  EvaluationResources second_context =
      make_trusted_local_v2_resources(no_failure);
  std::array<Value, 1> first_child{{make_int_value(1)}};
  std::array<Value, 1> second_child{{make_int_value(2)}};
  TupleConstructionResult first_tuple = make_tuple_value(
      first_context, first_child, tuple_location, "first-context");
  TupleConstructionResult second_tuple = make_tuple_value(
      second_context, second_child, tuple_location, "second-context");
  REQUIRE(first_tuple.ok);
  REQUIRE(second_tuple.ok);
  const ValueReleaseResult wrong_context =
      release_value_reservations(second_context, first_tuple.value);
  CHECK_FALSE(wrong_context.ok);
  CHECK(wrong_context.error == ValueReleaseError::resource_context_mismatch);
  CHECK(first_tuple.value.claimed);
  CHECK(first_context.live_evaluation_bytes == 16U);
  CHECK(second_context.live_evaluation_bytes == 16U);
  CHECK(release_value_reservations(first_context, first_tuple.value).ok);
  CHECK(release_value_reservations(second_context, second_tuple.value).ok);
  CHECK(first_context.live_evaluation_bytes == 0U);
  CHECK(second_context.live_evaluation_bytes == 0U);

  VectorAllocationResult first_vector = allocate_vector(
      first_context, ScalarType::integer, 1U, 0U, tuple_location,
      "first-vector-context");
  VectorAllocationResult second_vector = allocate_vector(
      second_context, ScalarType::integer, 1U, 0U, tuple_location,
      "second-vector-context");
  REQUIRE(first_vector.ok);
  REQUIRE(second_vector.ok);
  const ValueReleaseResult wrong_vector_context =
      release_vector_reservation(second_context, first_vector.value);
  CHECK_FALSE(wrong_vector_context.ok);
  CHECK(wrong_vector_context.error ==
        ValueReleaseError::resource_context_mismatch);
  CHECK(first_vector.value.claimed);
  CHECK(first_context.live_evaluation_bytes == sizeof(std::int64_t));
  CHECK(second_context.live_evaluation_bytes == sizeof(std::int64_t));
  CHECK(release_vector_reservation(first_context, first_vector.value).ok);
  CHECK(release_vector_reservation(second_context, second_vector.value).ok);

  VectorAllocationResult malformed_vector = allocate_vector(
      first_context, ScalarType::integer, 1U, 0U, tuple_location,
      "malformed-vector-release");
  REQUIRE(malformed_vector.ok);
  std::int64_t *malformed_storage =
      malformed_vector.value.vector.integers.get();
  malformed_vector.value.scalar.integer = 1;
  const std::size_t live_before_malformed_release =
      first_context.live_evaluation_bytes;
  const ValueReleaseResult malformed_release =
      release_vector_reservation(first_context, malformed_vector.value);
  CHECK_FALSE(malformed_release.ok);
  CHECK(malformed_release.invariant == ValueInvariant::inactive_scalar_field);
  CHECK(malformed_vector.value.claimed);
  CHECK(malformed_vector.value.vector.integers.get() == malformed_storage);
  CHECK(first_context.live_evaluation_bytes == live_before_malformed_release);
  malformed_vector.value.scalar.integer = 0;
  CHECK(release_vector_reservation(first_context, malformed_vector.value).ok);
  CHECK(first_context.live_evaluation_bytes == 0U);

  EvaluationResources move_source =
      make_trusted_local_v2_resources(no_failure);
  {
    EvaluationResources move_destination = std::move(move_source);
    VectorAllocationResult moved_context_vector = allocate_vector(
        move_destination, ScalarType::integer, 1U, 0U, tuple_location,
        "moved-resource-context");
    REQUIRE(moved_context_vector.ok);
    CHECK(move_source.live_evaluation_bytes == sizeof(std::int64_t));
    CHECK(release_vector_reservation(move_source, moved_context_vector.value).ok);
  }
  CHECK(move_source.live_evaluation_bytes == 0U);
  VectorAllocationResult reused_move_source = allocate_vector(
      move_source, ScalarType::integer, 1U, 0U, tuple_location,
      "reused-moved-resource-context");
  REQUIRE(reused_move_source.ok);
  CHECK(release_vector_reservation(move_source, reused_move_source.value).ok);
}

TEST_CASE("TUP-005-FORMAT") {
  EvaluationResources resources = make_trusted_local_v2_resources(no_failure);
  std::array<Value, 0> no_children{};
  TupleConstructionResult empty = make_tuple_value(
      resources, no_children, tuple_location, "empty-tuple");
  REQUIRE(empty.ok);
  CHECK(valid_value_text(empty.value) == "[]");
  CHECK(resources.live_evaluation_bytes == 0U);
  CHECK(resources.reservation_ordinal == 0U);

  std::array<Value, 1> singleton_child{{make_int_value(7)}};
  TupleConstructionResult singleton = make_tuple_value(
      resources, singleton_child, tuple_location, "singleton-tuple");
  REQUIRE(singleton.ok);
  CHECK(valid_value_text(singleton.value) == "[7]");

  Value deep = move_value(singleton.value);
  constexpr std::size_t depth = 4096U;
  for (std::size_t index = 1U; index < depth; ++index) {
    std::array<Value, 1> child{{move_value(deep)}};
    TupleConstructionResult wrapped = make_tuple_value(
        resources, child, tuple_location, "deep-tuple");
    REQUIRE(wrapped.ok);
    deep = move_value(wrapped.value);
  }
  const ValueFormattingResult deep_text = format_value(deep);
  REQUIRE(deep_text.ok);
  CHECK(deep_text.formatted.size() == depth * 2U + 1U);
  CHECK(deep_text.formatted.front() == '[');
  CHECK(deep_text.formatted[depth] == '7');
  CHECK(deep_text.formatted.back() == ']');
  CHECK(release_value_reservations(resources, deep).ok);
  CHECK(resources.live_evaluation_bytes == 0U);
}

TEST_CASE("TUP-006-PROFILE-IDENTITY") {
  const EvaluationResources trusted_v1 =
      make_trusted_local_resources(no_failure);
  const EvaluationResources trusted_v2 =
      make_trusted_local_v2_resources(no_failure);
  CHECK(trusted_v1.profile == ExecutionProfile::trusted_local_v1);
  CHECK(trusted_v2.profile == ExecutionProfile::trusted_local_v2);
  CHECK(execution_profile_name(trusted_v1.profile) == "trusted-local-v1");
  CHECK(execution_profile_name(trusted_v2.profile) == "trusted-local-v2");
  CHECK_FALSE(trusted_v2.limits.max_tuple_table_bytes.has_value());

  EvaluationResources bounded_v2 = make_bounded_v2_resources(
      v2_limits(std::nullopt, std::nullopt, std::nullopt, 0U), no_failure);
  CHECK(bounded_v2.profile == ExecutionProfile::bounded_v2);
  CHECK(execution_profile_name(bounded_v2.profile) == "bounded-v2");

  std::array<Value, 0> empty_children{};
  const TupleConstructionResult empty = make_tuple_value(
      bounded_v2, empty_children, tuple_location, "empty-tuple");
  CHECK(empty.ok);

  EvaluationResources v1 = make_trusted_local_resources(no_failure);
  std::array<Value, 1> v1_child{{make_int_value(1)}};
  const TupleConstructionResult refused = make_tuple_value(
      v1, v1_child, tuple_location, "v1-tuple");
  CHECK_FALSE(refused.ok);
  CHECK(refused.error.kind == ErrorKind::profile_error);
  REQUIRE(refused.error.profile.has_value());
  CHECK(refused.error.profile->reason ==
        ProfileErrorReason::unsupported_value_kind);
  CHECK(refused.error.profile->profile_name == "trusted-local-v1");
  CHECK(refused.error.profile->value_kind == TypeKind::tuple);
  CHECK(v1_child[0].claimed);
  CHECK(v1.reservation_ordinal == 0U);

  EvaluationResources bounded_without_limits = make_bounded_v2_resources(
      v2_limits(std::nullopt, std::nullopt, std::nullopt, std::nullopt),
      no_failure);
  const TupleReservationResult missing_limit = reserve_tuple_table(
      bounded_without_limits, 0U, tuple_location, "invalid-bounded-v2");
  CHECK_FALSE(missing_limit.ok);
  CHECK(missing_limit.error.kind == ErrorKind::invalid_execution_profile);
  CHECK(bounded_without_limits.reservation_ordinal == 0U);

  EvaluationResources trusted_with_limit =
      make_trusted_local_v2_resources(no_failure);
  trusted_with_limit.limits.max_tuple_table_bytes = 0U;
  const TupleReservationResult unexpected_limit = reserve_tuple_table(
      trusted_with_limit, 0U, tuple_location, "invalid-trusted-v2");
  CHECK_FALSE(unexpected_limit.ok);
  CHECK(unexpected_limit.error.kind ==
        ErrorKind::invalid_execution_profile);

  EvaluationResources zero_tuple_limit = make_bounded_v2_resources(
      v2_limits(std::nullopt, std::nullopt, std::nullopt, 0U), no_failure);
  const TupleReservationResult accepted_zero = reserve_tuple_table(
      zero_tuple_limit, 0U, tuple_location, "zero-tuple-limit");
  CHECK(accepted_zero.ok);
  const TupleReservationResult refused_positive = reserve_tuple_table(
      zero_tuple_limit, 1U, tuple_location, "zero-tuple-limit");
  CHECK_FALSE(refused_positive.ok);
  REQUIRE(refused_positive.error.resource.has_value());
  CHECK(refused_positive.error.resource->reason ==
        ResourceErrorReason::profile_limit);
  CHECK(refused_positive.error.resource->limit_kind ==
        std::optional<ResourceLimitKind>{
            ResourceLimitKind::max_tuple_table_bytes});

  EvaluationResources vector_v1 = make_trusted_local_resources(no_failure);
  EvaluationResources vector_v2 =
      make_trusted_local_v2_resources(no_failure);
  VectorAllocationResult allocated_v1 = allocate_vector(
      vector_v1, ScalarType::integer, 2U, 3U, tuple_location,
      "profile-equivalence");
  VectorAllocationResult allocated_v2 = allocate_vector(
      vector_v2, ScalarType::integer, 2U, 3U, tuple_location,
      "profile-equivalence");
  REQUIRE(allocated_v1.ok);
  REQUIRE(allocated_v2.ok);
  CHECK(vector_v1.live_evaluation_bytes == vector_v2.live_evaluation_bytes);
  CHECK(vector_v1.work_units == vector_v2.work_units);
  CHECK(vector_v1.reservation_ordinal == vector_v2.reservation_ordinal);
  release_vector_reservation(vector_v1, allocated_v1.value);
  release_vector_reservation(vector_v2, allocated_v2.value);
}

TEST_CASE("TUP-007-TABLE-CHARGE") {
  EvaluationResources exact = make_bounded_v2_resources(
      v2_limits(std::nullopt, 32U, std::nullopt, 32U), no_failure);
  std::array<Value, 2> exact_children{{make_int_value(1), make_int_value(2)}};
  TupleConstructionResult accepted = make_tuple_value(
      exact, exact_children, tuple_location, "tuple-table");
  REQUIRE(accepted.ok);
  CHECK(exact.live_evaluation_bytes == 32U);
  CHECK(exact.work_units == 0U);

  std::array<Value, 1> one_more{{make_int_value(3)}};
  TupleConstructionResult one_past = make_tuple_value(
      exact, one_more, tuple_location, "tuple-table");
  CHECK_FALSE(one_past.ok);
  REQUIRE(one_past.error.resource.has_value());
  CHECK(one_past.error.resource->reason == ResourceErrorReason::profile_limit);
  CHECK(*one_past.error.resource->limit_kind ==
        ResourceLimitKind::max_live_evaluation_bytes);
  CHECK(*one_past.error.resource->usage_before == 32U);
  CHECK(*one_past.error.resource->refused_charge == 16U);
  CHECK(one_more[0].claimed);
  CHECK(exact.reservation_ordinal == 1U);

  EvaluationResources tuple_one_past = make_bounded_v2_resources(
      v2_limits(std::nullopt, std::nullopt, std::nullopt, 31U), no_failure);
  std::array<Value, 2> too_wide{{make_int_value(1), make_int_value(2)}};
  const TupleConstructionResult table_refused = make_tuple_value(
      tuple_one_past, too_wide, tuple_location, "tuple-table");
  CHECK_FALSE(table_refused.ok);
  REQUIRE(table_refused.error.resource.has_value());
  CHECK(*table_refused.error.resource->limit_kind ==
        ResourceLimitKind::max_tuple_table_bytes);
  CHECK(tuple_one_past.reservation_ordinal == 0U);

  const TupleReservationResult overflow = reserve_tuple_table(
      tuple_one_past, std::numeric_limits<std::size_t>::max(), tuple_location,
      "tuple-overflow");
  CHECK_FALSE(overflow.ok);
  REQUIRE(overflow.error.resource.has_value());
  CHECK(overflow.error.resource->reason == ResourceErrorReason::size_overflow);
  CHECK(tuple_one_past.reservation_ordinal == 0U);

  CHECK(release_value_reservations(exact, accepted.value).ok);
  CHECK(exact.live_evaluation_bytes == 0U);
}

TEST_CASE("TUP-008-ALLOCATION-ORDINAL") {
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  EvaluationResources vector_overflow =
      make_trusted_local_v2_resources(no_failure);
  vector_overflow.reservation_ordinal = maximum;
  const VectorAllocationResult vector_ordinal_overflow = allocate_vector(
      vector_overflow, ScalarType::boolean, 1U, 0U, tuple_location,
      "vector-ordinal-overflow");
  CHECK_FALSE(vector_ordinal_overflow.ok);
  REQUIRE(vector_ordinal_overflow.error.resource.has_value());
  CHECK(vector_ordinal_overflow.error.resource->reason ==
        ResourceErrorReason::size_overflow);
  CHECK(vector_overflow.reservation_ordinal == maximum);

  EvaluationResources workspace_overflow =
      make_trusted_local_v2_resources(no_failure);
  workspace_overflow.reservation_ordinal = maximum;
  const WorkspaceReservationResult workspace_ordinal_overflow =
      reserve_workspace(workspace_overflow, 1U, 0U, tuple_location,
                        "workspace-ordinal-overflow");
  CHECK_FALSE(workspace_ordinal_overflow.ok);
  REQUIRE(workspace_ordinal_overflow.error.resource.has_value());
  CHECK(workspace_ordinal_overflow.error.resource->reason ==
        ResourceErrorReason::size_overflow);
  CHECK(workspace_overflow.reservation_ordinal == maximum);

  EvaluationResources tuple_overflow =
      make_trusted_local_v2_resources(no_failure);
  tuple_overflow.reservation_ordinal = maximum;
  const TupleReservationResult tuple_ordinal_overflow = reserve_tuple_table(
      tuple_overflow, 1U, tuple_location, "tuple-ordinal-overflow");
  CHECK_FALSE(tuple_ordinal_overflow.ok);
  REQUIRE(tuple_ordinal_overflow.error.resource.has_value());
  CHECK(tuple_ordinal_overflow.error.resource->reason ==
        ResourceErrorReason::size_overflow);
  CHECK(tuple_overflow.reservation_ordinal == maximum);

  EvaluationResources resources = make_trusted_local_v2_resources(
      AllocationFailureInjection{2U});
  VectorAllocationResult vector = allocate_vector(
      resources, ScalarType::integer, 1U, 0U, tuple_location, "vector-first");
  REQUIRE(vector.ok);
  WorkspaceReservationResult workspace = reserve_workspace(
      resources, 1U, 0U, tuple_location, "workspace-second");
  REQUIRE(workspace.ok);
  CHECK(resources.reservation_ordinal == 2U);

  std::array<Value, 1> tuple_child{{make_int_value(1)}};
  TupleConstructionResult injected = make_tuple_value(
      resources, tuple_child, tuple_location, "tuple-third");
  CHECK_FALSE(injected.ok);
  REQUIRE(injected.error.resource.has_value());
  CHECK(injected.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  CHECK(*injected.error.resource->allocation_ordinal == 2U);
  CHECK(resources.reservation_ordinal == 3U);
  CHECK(resources.live_evaluation_bytes == 9U);
  CHECK(tuple_child[0].claimed);

  release_vector_reservation(resources, vector.value);
  release_workspace(resources, workspace.reservation);
  CHECK(resources.live_evaluation_bytes == 0U);

  for (std::size_t failed_ordinal = 0U; failed_ordinal < 3U;
       ++failed_ordinal) {
    CAPTURE(failed_ordinal);
    EvaluationResources nested_resources =
        make_trusted_local_v2_resources(
            AllocationFailureInjection{failed_ordinal});
    std::array<Value, 1> first_children{{make_int_value(1)}};
    TupleConstructionResult first = make_tuple_value(
        nested_resources, first_children, tuple_location, "nested-first");
    if (failed_ordinal == 0U) {
      CHECK_FALSE(first.ok);
      REQUIRE(first.error.resource.has_value());
      CHECK(first.error.resource->allocation_ordinal ==
            std::optional<std::size_t>{failed_ordinal});
      CHECK(nested_resources.reservation_ordinal == 1U);
      CHECK(nested_resources.live_evaluation_bytes == 0U);
      continue;
    }
    REQUIRE(first.ok);

    std::array<Value, 2> second_children{{make_int_value(2),
                                         make_int_value(3)}};
    TupleConstructionResult second = make_tuple_value(
        nested_resources, second_children, tuple_location, "nested-second");
    if (failed_ordinal == 1U) {
      CHECK_FALSE(second.ok);
      REQUIRE(second.error.resource.has_value());
      CHECK(second.error.resource->allocation_ordinal ==
            std::optional<std::size_t>{failed_ordinal});
      CHECK(release_value_reservations(nested_resources, first.value).ok);
      CHECK(nested_resources.reservation_ordinal == 2U);
      CHECK(nested_resources.live_evaluation_bytes == 0U);
      continue;
    }
    REQUIRE(second.ok);

    std::array<Value, 2> outer_children{{move_value(first.value),
                                        move_value(second.value)}};
    TupleConstructionResult outer = make_tuple_value(
        nested_resources, outer_children, tuple_location, "nested-outer");
    CHECK_FALSE(outer.ok);
    REQUIRE(outer.error.resource.has_value());
    CHECK(outer.error.resource->allocation_ordinal ==
          std::optional<std::size_t>{failed_ordinal});
    CHECK(outer_children[0].claimed);
    CHECK(outer_children[1].claimed);
    CHECK(release_value_reservations(nested_resources, outer_children[1]).ok);
    CHECK(release_value_reservations(nested_resources, outer_children[0]).ok);
    CHECK(nested_resources.reservation_ordinal == 3U);
    CHECK(nested_resources.live_evaluation_bytes == 0U);
  }

  for (std::size_t failed_ordinal = 0U; failed_ordinal < 2U;
       ++failed_ordinal) {
    CAPTURE(failed_ordinal);
    EvaluationResources empty_nested_resources =
        make_trusted_local_v2_resources(
            AllocationFailureInjection{failed_ordinal});
    std::array<Value, 0> empty_children{};
    TupleConstructionResult empty = make_tuple_value(
        empty_nested_resources, empty_children, tuple_location,
        "empty-nested-first");
    REQUIRE(empty.ok);
    CHECK(empty_nested_resources.reservation_ordinal == 0U);

    std::array<Value, 1> singleton_children{{make_int_value(1)}};
    TupleConstructionResult singleton = make_tuple_value(
        empty_nested_resources, singleton_children, tuple_location,
        "empty-nested-singleton");
    if (failed_ordinal == 0U) {
      CHECK_FALSE(singleton.ok);
      REQUIRE(singleton.error.resource.has_value());
      CHECK(singleton.error.resource->allocation_ordinal ==
            std::optional<std::size_t>{failed_ordinal});
      CHECK(release_value_reservations(empty_nested_resources, empty.value).ok);
      CHECK(empty_nested_resources.live_evaluation_bytes == 0U);
      continue;
    }
    REQUIRE(singleton.ok);

    std::array<Value, 2> outer_children{{move_value(empty.value),
                                        move_value(singleton.value)}};
    TupleConstructionResult outer = make_tuple_value(
        empty_nested_resources, outer_children, tuple_location,
        "empty-nested-outer");
    CHECK_FALSE(outer.ok);
    REQUIRE(outer.error.resource.has_value());
    CHECK(outer.error.resource->allocation_ordinal ==
          std::optional<std::size_t>{failed_ordinal});
    CHECK(release_value_reservations(empty_nested_resources,
                                     outer_children[1])
              .ok);
    CHECK(release_value_reservations(empty_nested_resources,
                                     outer_children[0])
              .ok);
    CHECK(empty_nested_resources.reservation_ordinal == 2U);
    CHECK(empty_nested_resources.live_evaluation_bytes == 0U);
  }
}

TEST_CASE("TUP-009-HOST-ALLOCATION-FAILURES") {
  std::array<TypeArena, 2> elements{{make_scalar_type(ScalarType::integer),
                                     make_vector_type(ScalarType::boolean)}};
  std::size_t construction_failures = 0U;
  bool construction_succeeded = false;
  for (std::size_t ordinal = 0U; ordinal < 16U; ++ordinal) {
    HostAllocationFailureInjection failure{ordinal, 0U};
    const TypeConstructionResult result = make_tuple_type(elements, failure);
    if (result.ok) {
      construction_succeeded = true;
      break;
    }
    ++construction_failures;
    CHECK(result.invariant == TypeInvariant::none);
    CHECK(result.resource_error ==
          HostResourceErrorReason::allocation_unavailable);
  }
  CHECK(construction_failures >= 1U);
  CHECK(construction_succeeded);

  const TypeConstructionResult pair = make_tuple_type(elements);
  REQUIRE(pair.ok);
  const TypeArena &pair_type = pair.type;
  HostAllocationFailureInjection validation_failure{0U, 0U};
  const TypeValidationResult invalid_for_resources =
      validate_type(pair_type, validation_failure);
  CHECK_FALSE(invalid_for_resources.ok);
  CHECK(invalid_for_resources.invariant == TypeInvariant::none);
  CHECK(invalid_for_resources.resource_error ==
        HostResourceErrorReason::allocation_unavailable);

  std::size_t equality_failures = 0U;
  bool equality_succeeded = false;
  for (std::size_t ordinal = 0U; ordinal < 16U; ++ordinal) {
    HostAllocationFailureInjection equality_failure{ordinal, 0U};
    const TypeEqualityResult equality =
        structural_type_equal(pair_type, pair_type, equality_failure);
    if (equality.ok) {
      CHECK(equality.equal);
      equality_succeeded = true;
      break;
    }
    ++equality_failures;
    CHECK_FALSE(equality.equal);
    CHECK(equality.invariant == TypeInvariant::none);
    CHECK(equality.resource_error ==
          HostResourceErrorReason::allocation_unavailable);
  }
  CHECK(equality_failures >= 1U);
  CHECK(equality_succeeded);

  std::size_t type_format_failures = 0U;
  bool type_format_succeeded = false;
  for (std::size_t ordinal = 0U; ordinal < 16U; ++ordinal) {
    HostAllocationFailureInjection type_format_failure{ordinal, 0U};
    const TypeFormattingResult type_format =
        format_type(pair_type, type_format_failure);
    if (type_format.ok) {
      CHECK(type_format.formatted == "Tuple<Int, Vector<Bool>>");
      type_format_succeeded = true;
      break;
    }
    ++type_format_failures;
    CHECK(type_format.invariant == TypeInvariant::none);
    CHECK(type_format.resource_error ==
          HostResourceErrorReason::allocation_unavailable);
  }
  CHECK(type_format_failures >= 1U);
  CHECK(type_format_succeeded);

  EvaluationResources resources =
      make_trusted_local_v2_resources(no_failure);
  std::array<Value, 2> children{{make_int_value(1), make_bool_value(true)}};
  TupleConstructionResult tuple = make_tuple_value(
      resources, children, tuple_location, "host-allocation-failures");
  REQUIRE(tuple.ok);

  HostAllocationFailureInjection value_validation_failure{0U, 0U};
  const ValueValidationResult value_validation =
      validate_value(tuple.value, value_validation_failure);
  CHECK_FALSE(value_validation.ok);
  CHECK(value_validation.invariant == ValueInvariant::none);
  CHECK(value_validation.resource_error ==
        HostResourceErrorReason::allocation_unavailable);

  HostAllocationFailureInjection value_type_validation_failure{0U, 0U};
  const ValueTypeResult value_type_validation =
      value_type(tuple.value, value_type_validation_failure);
  CHECK_FALSE(value_type_validation.ok);
  CHECK(value_type_validation.invariant == ValueInvariant::none);
  CHECK(value_type_validation.error == ValueAccessError::none);
  CHECK(value_type_validation.resource_error ==
        HostResourceErrorReason::allocation_unavailable);

  HostAllocationFailureInjection value_type_construction_failure{1U, 0U};
  const ValueTypeResult value_type_construction =
      value_type(tuple.value, value_type_construction_failure);
  CHECK_FALSE(value_type_construction.ok);
  CHECK(value_type_construction.invariant == ValueInvariant::none);
  CHECK(value_type_construction.error == ValueAccessError::none);
  CHECK(value_type_construction.resource_error ==
        HostResourceErrorReason::allocation_unavailable);

  HostAllocationFailureInjection value_type_success{2U, 0U};
  const ValueTypeResult constructed_value_type =
      value_type(tuple.value, value_type_success);
  REQUIRE(constructed_value_type.ok);
  CHECK(valid_type_text(constructed_value_type.type) == "Tuple<Int, Bool>");

  HostAllocationFailureInjection value_format_failure{1U, 0U};
  const ValueFormattingResult value_format =
      format_value(tuple.value, value_format_failure);
  CHECK_FALSE(value_format.ok);
  CHECK(value_format.error == ValueFormatError::none);
  CHECK(value_format.resource_error ==
        HostResourceErrorReason::allocation_unavailable);
  CHECK(release_value_reservations(resources, tuple.value).ok);

  EvaluationResources metadata_resources =
      make_trusted_local_v2_resources(no_failure);
  std::array<Value, 2> metadata_children{{make_int_value(1),
                                         make_bool_value(true)}};
  HostAllocationFailureInjection metadata_failure{0U, 0U};
  const TupleConstructionResult metadata_result = make_tuple_value(
      metadata_resources, metadata_children, tuple_location,
      "host-metadata-failure", metadata_failure);
  CHECK_FALSE(metadata_result.ok);
  CHECK(metadata_result.invariant == ValueInvariant::none);
  REQUIRE(metadata_result.error.resource.has_value());
  CHECK(metadata_result.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  CHECK(metadata_result.error.resource->allocation_ordinal ==
        std::optional<std::size_t>{0U});
  CHECK(metadata_resources.reservation_ordinal == 1U);
  CHECK(metadata_resources.live_evaluation_bytes == 0U);
  CHECK(metadata_children[0].claimed);
  CHECK(metadata_children[1].claimed);

  EvaluationResources destroy_resources =
      make_trusted_local_v2_resources(no_failure);
  std::array<Value, 1> destroy_children{{make_int_value(1)}};
  TupleConstructionResult destroy_tuple = make_tuple_value(
      destroy_resources, destroy_children, tuple_location,
      "host-destroy-failure");
  REQUIRE(destroy_tuple.ok);
  HostAllocationFailureInjection destroy_failure{0U, 0U};
  const ValueDestructionResult destroy_result =
      destroy_value(destroy_tuple.value, destroy_failure);
  CHECK_FALSE(destroy_result.ok);
  CHECK(destroy_result.invariant == ValueInvariant::none);
  CHECK(destroy_result.resource_error ==
        HostResourceErrorReason::allocation_unavailable);
  CHECK(destroy_tuple.value.claimed);
  CHECK(destroy_resources.live_evaluation_bytes == 16U);
  CHECK(destroy_value(destroy_tuple.value).ok);
  CHECK(destroy_resources.live_evaluation_bytes == 0U);

  EvaluationResources release_resources =
      make_trusted_local_v2_resources(no_failure);
  std::array<Value, 1> release_children{{make_int_value(1)}};
  TupleConstructionResult release_tuple = make_tuple_value(
      release_resources, release_children, tuple_location,
      "host-release-failure");
  REQUIRE(release_tuple.ok);
  HostAllocationFailureInjection release_failure{0U, 0U};
  const ValueReleaseResult release_result = release_value_reservations(
      release_resources, release_tuple.value, release_failure);
  CHECK_FALSE(release_result.ok);
  CHECK(release_result.invariant == ValueInvariant::none);
  CHECK(release_result.resource_error ==
        HostResourceErrorReason::allocation_unavailable);
  CHECK(release_tuple.value.claimed);
  CHECK(release_resources.live_evaluation_bytes == 16U);
  CHECK(release_value_reservations(release_resources, release_tuple.value).ok);
  CHECK(release_resources.live_evaluation_bytes == 0U);
}

TEST_CASE("TUP-012-DIRECT-PRESERVATION") {
  EvaluationResources tuple_resources =
      make_trusted_local_v2_resources(no_failure);
  std::array<Value, 2> tuple_children{{make_int_value(1), make_int_value(2)}};
  TupleConstructionResult tuple = make_tuple_value(
      tuple_resources, tuple_children, tuple_location, "direct-tuple");
  REQUIRE(tuple.ok);

  EvaluationResources application_resources =
      make_trusted_local_v2_resources(no_failure);
  PrimitiveApplicationContext context{application_resources, 0U};
  const PrimitiveDescriptor &inc = *find_primitive(PrimitiveId::inc);
  std::array<Value, 1> arguments{{move_value(tuple.value)}};
  const PrimitiveApplicationResult result =
      apply_primitive(context, inc, arguments, tuple_location);
  CHECK_FALSE(result.ok);
  CHECK(result.error.kind == ErrorKind::type_mismatch);
  REQUIRE(result.error.type.has_value());
  REQUIRE(result.error.type->actual_arguments.size() == 1U);
  CHECK(valid_type_text(result.error.type->actual_arguments[0]) ==
        "Tuple<Int, Int>");
  CHECK(result.error.argument_position == std::optional<std::size_t>{1U});
  CHECK(context.scalar_kernel_invocations == 0U);
  CHECK(application_resources.live_evaluation_bytes == 0U);

  Value owned_argument = move_value(arguments[0]);
  CHECK(release_value_reservations(tuple_resources, owned_argument).ok);

  std::array<Value, 1> bool_child{{make_bool_value(true)}};
  TupleConstructionResult bool_tuple = make_tuple_value(
      tuple_resources, bool_child, tuple_location, "direct-bool-tuple");
  REQUIRE(bool_tuple.ok);
  std::array<Value, 1> bool_argument{{move_value(bool_tuple.value)}};
  const PrimitiveDescriptor &logical_not =
      *find_primitive(PrimitiveId::logical_not);
  const PrimitiveApplicationResult bool_type_error = apply_primitive(
      context, logical_not, bool_argument, tuple_location);
  CHECK_FALSE(bool_type_error.ok);
  CHECK(bool_type_error.error.kind == ErrorKind::type_mismatch);
  CHECK(bool_type_error.error.argument_position ==
        std::optional<std::size_t>{1U});
  REQUIRE(bool_type_error.error.type.has_value());
  REQUIRE(bool_type_error.error.type->actual_arguments.size() == 1U);
  CHECK(valid_type_text(bool_type_error.error.type->actual_arguments[0]) ==
        "Tuple<Bool>");
  Value owned_bool_argument = move_value(bool_argument[0]);
  CHECK(release_value_reservations(tuple_resources, owned_bool_argument).ok);

  Value malformed = make_int_value(1);
  malformed.tuple.first_child = 1U;
  std::array<Value, 1> malformed_argument{{move_value(malformed)}};
  const PrimitiveApplicationResult invalid =
      apply_primitive(context, inc, malformed_argument, tuple_location);
  CHECK_FALSE(invalid.ok);
  CHECK(invalid.error.kind == ErrorKind::invalid_value);
  REQUIRE(invalid.error.value.has_value());
  CHECK(invalid.error.value->invariant == ValueInvariant::inactive_tuple_field);
  CHECK(invalid.error.value->path.empty());
  CHECK(invalid.error.value->node_index == std::optional<std::size_t>{0U});
  CHECK(context.scalar_kernel_invocations == 0U);

  const PrimitiveDescriptor &add = *find_primitive(PrimitiveId::add);
  const PrimitiveApplicationResult arity_first =
      apply_primitive(context, add, malformed_argument, tuple_location);
  CHECK_FALSE(arity_first.ok);
  CHECK(arity_first.error.kind == ErrorKind::arity_error);
  CHECK_FALSE(arity_first.error.value.has_value());
}

TEST_CASE("TUP-016-DEEP-NESTING") {
  TypeArena deep_type = make_scalar_type(ScalarType::integer);
  for (std::size_t depth = 0U; depth < 1024U; ++depth) {
    const std::array<TypeArena, 1> child{{std::move(deep_type)}};
    TypeConstructionResult next = make_tuple_type(child);
    REQUIRE(next.ok);
    deep_type = std::move(next.type);
  }
  CHECK(validate_type(deep_type).ok);
  CHECK(format_type(deep_type).ok);

  EvaluationResources resources = make_trusted_local_v2_resources(no_failure);
  Value deep_value = make_int_value(1);
  for (std::size_t depth = 0U; depth < 1024U; ++depth) {
    std::array<Value, 1> child{{move_value(deep_value)}};
    TupleConstructionResult next = make_tuple_value(
        resources, child, tuple_location, "deep-tuple-foundation");
    REQUIRE(next.ok);
    deep_value = move_value(next.value);
  }
  CHECK(validate_value(deep_value).ok);
  CHECK(format_value(deep_value).ok);
  CHECK(release_value_reservations(resources, deep_value).ok);
  CHECK(resources.live_evaluation_bytes == 0U);
}

TEST_CASE("TUP-018-REGRESSION-PLATFORMS") {
  EvaluationResources resources = make_bounded_resources(
      ResourceLimits{128U, 4096U, 64U}, no_failure);
  VectorAllocationResult vector = copy_int_vector(
      resources, std::array<std::int64_t, 3>{1, 2, 3}, tuple_location,
      "tuple-regression-vector");
  REQUIRE(vector.ok);
  CHECK(format_value(vector.value).formatted == "(1 2 3)");
  CHECK(valid_type_text(value_type(vector.value).type) == "Vector<Int>");
  CHECK(resources.live_evaluation_bytes == 3U * sizeof(std::int64_t));
  release_vector_reservation(resources, vector.value);
  CHECK(resources.live_evaluation_bytes == 0U);

  const Value scalar = make_double_value(1.5);
  CHECK(validate_value(scalar).ok);
  CHECK(format_value(scalar).formatted == "1.5");
  const bool supported_size_width =
      sizeof(std::size_t) == 4U || sizeof(std::size_t) == 8U;
  CHECK(supported_size_width);
}

} // namespace bennu
