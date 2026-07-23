#ifndef BENNU_VALUE_HPP
#define BENNU_VALUE_HPP

#include "bennu/type.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bennu {

enum class ScalarType {
  boolean,
  integer,
  double_precision,
};

enum class ContainerKind {
  scalar,
  vector,
  tuple,
};

struct ScalarValue {
  ScalarType type;
  bool boolean;
  std::int64_t integer;
  double double_precision;
};

struct VectorValue {
  ScalarType element_type;
  std::unique_ptr<std::uint8_t, decltype(&std::free)> booleans;
  std::size_t boolean_count;
  std::unique_ptr<std::int64_t, decltype(&std::free)> integers;
  std::size_t integer_count;
  std::unique_ptr<double, decltype(&std::free)> doubles;
  std::size_t double_count;
  std::size_t canonical_bytes{0U};
  bool accounting_active{false};
  std::shared_ptr<std::size_t> accounting_owner{};
};

using TupleTableStorage =
    std::unique_ptr<std::byte, decltype(&std::free)>;

struct TupleTableReservation {
  TupleTableStorage storage{nullptr, &std::free};
  std::size_t element_count{0U};
  std::size_t canonical_bytes{0U};
  bool accounting_active{false};
  std::shared_ptr<std::size_t> accounting_owner{};
};

struct ValueNode {
  ContainerKind container;
  ScalarValue scalar;
  std::size_t first_child;
  std::size_t child_count;
  std::size_t tuple_reservation_index;
  std::size_t vector_payload_index;
};

struct TupleValue {
  std::vector<ValueNode> nodes{};
  std::vector<std::size_t> child_indexes{};
  std::size_t root_index{0U};
  std::vector<VectorValue> vector_payloads{};
  std::vector<TupleTableReservation> reservations{};
  TupleTableReservation root_reservation{};
  std::size_t first_child{0U};
  std::size_t child_count{0U};
};

struct Value {
  ContainerKind container;
  ScalarValue scalar;
  VectorValue vector;
  TupleValue tuple{};
  bool claimed{true};
};

enum class ValueInvariant {
  none,
  unknown_container,
  unknown_scalar_type,
  inactive_scalar_field,
  inactive_vector_payload,
  invalid_boolean_element,
  noncanonical_nan,
  empty_owner,
  invalid_value_root,
  nonfinal_value_root,
  inactive_tuple_field,
  invalid_tuple_range,
  overlapping_tuple_range,
  invalid_tuple_child_index,
  non_postorder_tuple_child,
  aliased_tuple_child,
  invalid_vector_payload_handle,
  aliased_vector_payload,
  orphan_value_node,
  orphan_tuple_edge,
  orphan_vector_payload_handle,
  missing_tuple_reservation,
  aliased_tuple_reservation,
  invalid_tuple_reservation_count,
  orphan_tuple_reservation,
};

enum class ValueAccessError {
  none,
  invalid_value,
  container_mismatch,
  index_out_of_bounds,
};

struct ValueValidationResult {
  bool ok;
  ValueInvariant invariant;
  ValueAccessError error{ValueAccessError::none};
  std::vector<std::size_t> path{};
  std::optional<std::size_t> node_index{};
  std::optional<std::size_t> edge_index{};
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

struct ScalarProjectionResult {
  bool ok;
  ScalarValue value;
  ValueAccessError error;
};

enum class ValueFormatError {
  none,
  invalid_value,
  conversion_failure,
};

struct ValueFormattingResult {
  bool ok;
  std::string formatted;
  ValueInvariant invariant;
  ValueFormatError error;
  std::vector<std::size_t> path{};
  std::optional<std::size_t> node_index{};
  std::optional<std::size_t> edge_index{};
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

struct BorrowedValueView {
  const Value *owner;
  std::size_t node_index;
};

struct ValueTypeResult {
  bool ok;
  TypeArena type;
  ValueInvariant invariant;
  ValueAccessError error;
  std::vector<std::size_t> path{};
  std::optional<std::size_t> node_index{};
  std::optional<std::size_t> edge_index{};
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

struct ValueTupleArityResult {
  bool ok;
  std::size_t arity;
  ValueInvariant invariant;
  ValueAccessError error;
  std::vector<std::size_t> path{};
  std::optional<std::size_t> node_index{};
  std::optional<std::size_t> edge_index{};
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

struct ValueTupleElementResult {
  bool ok;
  BorrowedValueView view;
  ValueInvariant invariant;
  ValueAccessError error;
  std::vector<std::size_t> path{};
  std::optional<std::size_t> node_index{};
  std::optional<std::size_t> edge_index{};
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

struct ValueDestructionResult {
  bool ok;
  ValueInvariant invariant;
  std::vector<std::size_t> path{};
  std::optional<std::size_t> node_index{};
  std::optional<std::size_t> edge_index{};
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

Value make_bool_value(bool value);
Value make_int_value(std::int64_t value);
Value make_double_value(double value);
Value move_value(Value &source);
ValueValidationResult validate_value(const Value &value);
ValueValidationResult validate_value(
    const Value &value,
    HostAllocationFailureInjection &allocation_failure);
ValueValidationResult value_element_type(const Value &value,
                                         ScalarType &element_type);
ValueValidationResult value_rank(const Value &value, std::size_t &rank);
ValueValidationResult value_length(const Value &value, std::size_t &length);
ScalarProjectionResult project_scalar(const Value &value, std::size_t index);
ValueTypeResult value_type(const Value &value);
ValueTypeResult value_type(
    const Value &value,
    HostAllocationFailureInjection &allocation_failure);
ValueTypeResult value_type(BorrowedValueView view);
ValueTypeResult value_type(
    BorrowedValueView view,
    HostAllocationFailureInjection &allocation_failure);
ValueTupleArityResult value_tuple_arity(const Value &value);
ValueTupleElementResult value_tuple_element(const Value &value,
                                            std::size_t index);
ValueDestructionResult destroy_value(Value &value);
ValueDestructionResult destroy_value(
    Value &value,
    HostAllocationFailureInjection &allocation_failure);
ValueFormattingResult format_value(const Value &value);
ValueFormattingResult format_value(
    const Value &value,
    HostAllocationFailureInjection &allocation_failure);

} // namespace bennu

#endif
