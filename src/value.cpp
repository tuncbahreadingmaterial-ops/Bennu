#include "bennu/value.hpp"
#include "bennu/resources.hpp"

#include "doctest/doctest.h"

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <new>
#include <ostream>
#include <span>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bennu {

namespace {

constexpr std::uint64_t binary64_exponent_mask = UINT64_C(0x7ff0000000000000);
constexpr std::uint64_t binary64_fraction_mask = UINT64_C(0x000fffffffffffff);
constexpr std::uint64_t canonical_nan_bits = UINT64_C(0x7ff8000000000000);
static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<double>::is_iec559);

double normalize_double(double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & binary64_exponent_mask) == binary64_exponent_mask &&
      (bits & binary64_fraction_mask) != 0) {
    return std::bit_cast<double>(canonical_nan_bits);
  }
  return value;
}

bool is_positive_zero(double value) {
  return std::bit_cast<std::uint64_t>(value) == UINT64_C(0);
}

bool is_canonical_double(double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & binary64_exponent_mask) != binary64_exponent_mask ||
      (bits & binary64_fraction_mask) == 0) {
    return true;
  }
  return bits == canonical_nan_bits;
}

ScalarValue empty_scalar() {
  return ScalarValue{ScalarType::boolean, false, 0, 0.0};
}

VectorValue empty_vector() {
  return VectorValue{ScalarType::boolean,
                     {nullptr, &std::free},
                     0,
                     {nullptr, &std::free},
                     0,
                     {nullptr, &std::free},
                     0};
}

Value invalid_construction_value() {
  Value value{ContainerKind::scalar, empty_scalar(), empty_vector()};
  value.claimed = false;
  return value;
}

ValueValidationResult validate_scalar(const ScalarValue &scalar) {
  switch (scalar.type) {
  case ScalarType::boolean:
    if (scalar.integer != 0 || !is_positive_zero(scalar.double_precision)) {
      return ValueValidationResult{false,
                                   ValueInvariant::inactive_scalar_field};
    }
    return ValueValidationResult{true, ValueInvariant::none};
  case ScalarType::integer:
    if (scalar.boolean || !is_positive_zero(scalar.double_precision)) {
      return ValueValidationResult{false,
                                   ValueInvariant::inactive_scalar_field};
    }
    return ValueValidationResult{true, ValueInvariant::none};
  case ScalarType::double_precision:
    if (scalar.boolean || scalar.integer != 0) {
      return ValueValidationResult{false,
                                   ValueInvariant::inactive_scalar_field};
    }
    if (!is_canonical_double(scalar.double_precision)) {
      return ValueValidationResult{false, ValueInvariant::noncanonical_nan};
    }
    return ValueValidationResult{true, ValueInvariant::none};
  }
  return ValueValidationResult{false, ValueInvariant::unknown_scalar_type};
}

bool is_empty_scalar(const ScalarValue &scalar) {
  return scalar.type == ScalarType::boolean && !scalar.boolean &&
         scalar.integer == 0 && is_positive_zero(scalar.double_precision);
}

std::size_t active_vector_length(const VectorValue &vector) {
  switch (vector.element_type) {
  case ScalarType::boolean:
    return vector.boolean_count;
  case ScalarType::integer:
    return vector.integer_count;
  case ScalarType::double_precision:
    return vector.double_count;
  }
  return 0;
}

const void *vector_storage_address(const VectorValue &vector) {
  switch (vector.element_type) {
  case ScalarType::boolean:
    return vector.booleans.get();
  case ScalarType::integer:
    return vector.integers.get();
  case ScalarType::double_precision:
    return vector.doubles.get();
  }
  return nullptr;
}

void observe_lifetime(const ResourceLifetimeObserver &observer,
                      ResourceLifetimeEventKind kind,
                      ResourceStorageKind storage_kind,
                      std::optional<std::size_t> allocation_ordinal,
                      const void *storage, std::size_t canonical_bytes) {
  if (observer.record != nullptr) {
    observer.record(observer.context,
                    ResourceLifetimeEvent{kind, storage_kind,
                                          allocation_ordinal, storage,
                                          canonical_bytes});
  }
}

void reset_vector_storage(VectorValue &vector) {
  switch (vector.element_type) {
  case ScalarType::boolean:
    vector.booleans.reset();
    vector.boolean_count = 0U;
    break;
  case ScalarType::integer:
    vector.integers.reset();
    vector.integer_count = 0U;
    break;
  case ScalarType::double_precision:
    vector.doubles.reset();
    vector.double_count = 0U;
    break;
  }
}

bool append_double(std::string &formatted, double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & binary64_exponent_mask) == binary64_exponent_mask) {
    if ((bits & binary64_fraction_mask) != 0) {
      formatted += "nan";
      return true;
    }
    formatted += (bits >> 63U) != 0 ? "-inf" : "inf";
    return true;
  }
  if (bits == UINT64_C(0)) {
    formatted += "0.0";
    return true;
  }
  if (bits == UINT64_C(0x8000000000000000)) {
    formatted += "-0.0";
    return true;
  }

  std::array<char, 64> buffer{};
  const std::to_chars_result converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (converted.ec != std::errc{}) {
    return false;
  }
  const char *exponent = buffer.data();
  while (exponent != converted.ptr && *exponent != 'e' && *exponent != 'E') {
    ++exponent;
  }
  if (exponent == converted.ptr) {
    const char *point = buffer.data();
    while (point != converted.ptr && *point != '.') {
      ++point;
    }
    formatted.append(buffer.data(), converted.ptr);
    if (point == converted.ptr) {
      formatted += ".0";
    }
    return true;
  }

  formatted.append(buffer.data(),
                   static_cast<std::size_t>(exponent - buffer.data()));
  formatted += 'e';
  const char *digits = exponent + 1;
  bool negative = false;
  if (digits != converted.ptr && (*digits == '+' || *digits == '-')) {
    negative = *digits == '-';
    ++digits;
  }
  while (digits + 1 < converted.ptr && *digits == '0') {
    ++digits;
  }
  if (negative) {
    formatted += '-';
  }
  formatted.append(digits,
                   static_cast<std::size_t>(converted.ptr - digits));
  return true;
}

bool append_integer(std::string &formatted, std::int64_t value) {
  std::array<char, 32> buffer{};
  const std::to_chars_result converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (converted.ec != std::errc{}) {
    return false;
  }
  formatted.append(buffer.data(), converted.ptr);
  return true;
}

bool append_scalar(std::string &formatted, const ScalarValue &scalar) {
  switch (scalar.type) {
  case ScalarType::boolean:
    formatted += scalar.boolean ? "true" : "false";
    return true;
  case ScalarType::integer:
    return append_integer(formatted, scalar.integer);
  case ScalarType::double_precision:
    return append_double(formatted, scalar.double_precision);
  }
  return false;
}

} // namespace

Value make_bool_value(bool value) {
  return Value{ContainerKind::scalar,
               ScalarValue{ScalarType::boolean, value, 0, 0.0},
               empty_vector()};
}

Value make_int_value(std::int64_t value) {
  return Value{ContainerKind::scalar,
               ScalarValue{ScalarType::integer, false, value, 0.0},
               empty_vector()};
}

Value make_double_value(double value) {
  return Value{ContainerKind::scalar,
               ScalarValue{ScalarType::double_precision, false, 0,
                           normalize_double(value)},
               empty_vector()};
}

Value move_value(Value &source) {
  if (!source.claimed) {
    return invalid_construction_value();
  }
  Value result{source.container, source.scalar, std::move(source.vector),
               std::move(source.tuple), true};
  source = invalid_construction_value();
  return result;
}

ValueValidationResult validate_value(const Value &value) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return validate_value(value, allocation_failure);
}

ValueValidationResult validate_value(
    const Value &value,
    HostAllocationFailureInjection &allocation_failure) {
  std::vector<std::size_t> current_path;
  const auto invalid = [&current_path](
                           ValueInvariant invariant, std::size_t node_index,
                           std::optional<std::size_t> edge_index = std::nullopt) {
    ValueValidationResult result{false, invariant,
                                 ValueAccessError::invalid_value};
    result.path = current_path;
    result.node_index = node_index;
    result.edge_index = edge_index;
    return result;
  };
  const auto tuple_fields_empty = [](const TupleValue &tuple) {
    return tuple.nodes.empty() && tuple.child_indexes.empty() &&
           tuple.root_index == 0U && tuple.vector_payloads.empty() &&
           tuple.reservations.empty() &&
           tuple.root_reservation.storage.get() == nullptr &&
           tuple.root_reservation.element_count == 0U &&
           tuple.root_reservation.canonical_bytes == 0U &&
           !tuple.root_reservation.accounting_active &&
           tuple.root_reservation.accounting_owner == nullptr &&
           !tuple.root_reservation.allocation_ordinal.has_value() &&
           tuple.root_reservation.lifetime_observer.context == nullptr &&
           tuple.root_reservation.lifetime_observer.record == nullptr &&
           tuple.first_child == 0U && tuple.child_count == 0U;
  };
  const auto vector_validation = [&invalid](const VectorValue &vector,
                                             std::size_t node_index) {
    const auto accounting_valid = [&invalid, node_index](
                                      const VectorValue &payload,
                                      std::size_t count,
                                      std::size_t width) {
      if (count > std::numeric_limits<std::size_t>::max() / width) {
        return invalid(ValueInvariant::invalid_vector_payload_handle,
                       node_index);
      }
      const std::size_t bytes = count * width;
      if (payload.canonical_bytes != bytes ||
          (bytes == 0U && payload.accounting_active) ||
          (payload.accounting_active && payload.accounting_owner == nullptr) ||
          (bytes != 0U && payload.accounting_active &&
           !payload.allocation_ordinal.has_value()) ||
          (bytes == 0U && payload.allocation_ordinal.has_value()) ||
          (!payload.accounting_active && payload.accounting_owner != nullptr)) {
        return invalid(ValueInvariant::invalid_vector_payload_handle,
                       node_index);
      }
      return ValueValidationResult{true, ValueInvariant::none};
    };
    switch (vector.element_type) {
    case ScalarType::boolean:
      if (vector.integers.get() != nullptr || vector.integer_count != 0U ||
          vector.doubles.get() != nullptr || vector.double_count != 0U ||
          (vector.boolean_count == 0U) !=
              (vector.booleans.get() == nullptr)) {
        return invalid(ValueInvariant::inactive_vector_payload, node_index);
      }
      for (std::size_t index = 0U; index < vector.boolean_count; ++index) {
        if (vector.booleans.get()[index] > 1U) {
          return invalid(ValueInvariant::invalid_boolean_element, node_index);
        }
      }
      return accounting_valid(vector, vector.boolean_count, 1U);
    case ScalarType::integer:
      if (vector.booleans.get() != nullptr || vector.boolean_count != 0U ||
          vector.doubles.get() != nullptr || vector.double_count != 0U ||
          (vector.integer_count == 0U) !=
              (vector.integers.get() == nullptr)) {
        return invalid(ValueInvariant::inactive_vector_payload, node_index);
      }
      return accounting_valid(vector, vector.integer_count,
                              sizeof(std::int64_t));
    case ScalarType::double_precision:
      if (vector.booleans.get() != nullptr || vector.boolean_count != 0U ||
          vector.integers.get() != nullptr || vector.integer_count != 0U ||
          (vector.double_count == 0U) !=
              (vector.doubles.get() == nullptr)) {
        return invalid(ValueInvariant::inactive_vector_payload, node_index);
      }
      for (std::size_t index = 0U; index < vector.double_count; ++index) {
        if (!is_canonical_double(vector.doubles.get()[index])) {
          return invalid(ValueInvariant::noncanonical_nan, node_index);
        }
      }
      return accounting_valid(vector, vector.double_count, sizeof(double));
    }
    return invalid(ValueInvariant::unknown_scalar_type, node_index);
  };

  if (!value.claimed) {
    return invalid(ValueInvariant::empty_owner, 0U);
  }
  if (value.container == ContainerKind::scalar) {
    if (value.vector.element_type != ScalarType::boolean ||
        value.vector.booleans.get() != nullptr ||
        value.vector.boolean_count != 0U ||
        value.vector.integers.get() != nullptr ||
        value.vector.integer_count != 0U ||
        value.vector.doubles.get() != nullptr ||
        value.vector.double_count != 0U ||
        value.vector.canonical_bytes != 0U ||
        value.vector.accounting_active ||
        value.vector.accounting_owner != nullptr ||
        value.vector.allocation_ordinal.has_value() ||
        value.vector.lifetime_observer.context != nullptr ||
        value.vector.lifetime_observer.record != nullptr) {
      return invalid(ValueInvariant::inactive_vector_payload, 0U);
    }
    if (!tuple_fields_empty(value.tuple)) {
      return invalid(ValueInvariant::inactive_tuple_field, 0U);
    }
    const ValueValidationResult scalar_validation = validate_scalar(value.scalar);
    return scalar_validation.ok
               ? scalar_validation
               : invalid(scalar_validation.invariant, 0U);
  }
  if (value.container == ContainerKind::vector) {
    if (!is_empty_scalar(value.scalar)) {
      return invalid(ValueInvariant::inactive_scalar_field, 0U);
    }
    if (!tuple_fields_empty(value.tuple)) {
      return invalid(ValueInvariant::inactive_tuple_field, 0U);
    }
    return vector_validation(value.vector, 0U);
  }
  if (value.container != ContainerKind::tuple) {
    return invalid(ValueInvariant::unknown_container, 0U);
  }

  const std::size_t root_index = value.tuple.root_index;
  if (value.tuple.nodes.size() ==
      std::numeric_limits<std::size_t>::max()) {
    return invalid(ValueInvariant::invalid_value_root, root_index);
  }
  const std::size_t node_count = value.tuple.nodes.size() + 1U;
  if (root_index >= node_count) {
    return invalid(ValueInvariant::invalid_value_root, root_index);
  }
  if (root_index != node_count - 1U) {
    return invalid(ValueInvariant::nonfinal_value_root, root_index);
  }

  if (!is_empty_scalar(value.scalar)) {
    return invalid(ValueInvariant::inactive_scalar_field,
                   value.tuple.root_index);
  }
  if (value.vector.element_type != ScalarType::boolean ||
      value.vector.booleans.get() != nullptr ||
      value.vector.boolean_count != 0U ||
      value.vector.integers.get() != nullptr ||
      value.vector.integer_count != 0U ||
      value.vector.doubles.get() != nullptr ||
      value.vector.double_count != 0U || value.vector.canonical_bytes != 0U ||
      value.vector.accounting_active ||
      value.vector.accounting_owner != nullptr ||
      value.vector.allocation_ordinal.has_value() ||
      value.vector.lifetime_observer.context != nullptr ||
      value.vector.lifetime_observer.record != nullptr) {
    return invalid(ValueInvariant::inactive_vector_payload,
                   value.tuple.root_index);
  }

  if (allocation_failure.max_container_elements.has_value()) {
    const std::size_t maximum = *allocation_failure.max_container_elements;
    if (node_count > maximum || value.tuple.child_indexes.size() > maximum ||
        value.tuple.vector_payloads.size() > maximum ||
        value.tuple.reservations.size() ==
            std::numeric_limits<std::size_t>::max() ||
        value.tuple.reservations.size() + 1U > maximum) {
      ValueValidationResult result{false, ValueInvariant::none};
      result.resource_error = HostResourceErrorReason::size_overflow;
      return result;
    }
  }

  const std::size_t allocation_ordinal = allocation_failure.allocation_ordinal;
  if (allocation_ordinal == std::numeric_limits<std::size_t>::max() ||
      (allocation_failure.fail_at_allocation_ordinal.has_value() &&
       allocation_ordinal ==
           *allocation_failure.fail_at_allocation_ordinal)) {
    ValueValidationResult result{false, ValueInvariant::none};
    result.resource_error = HostResourceErrorReason::allocation_unavailable;
    return result;
  }
  ++allocation_failure.allocation_ordinal;
#if defined(__cpp_exceptions)
  try {
#endif

  std::vector<std::uint8_t> visited(node_count, 0U);
  std::vector<std::uint8_t> edge_owners(value.tuple.child_indexes.size(), 0U);
  std::vector<std::size_t> parent_count(node_count, 0U);
  std::vector<std::uint8_t> payload_owners(
      value.tuple.vector_payloads.size(), 0U);
  std::vector<std::uint8_t> reservation_owners(
      value.tuple.reservations.size() + 1U, 0U);
  struct ValidationWork {
    std::size_t node_index;
    std::vector<std::size_t> path;
  };
  std::vector<ValidationWork> stack{
      ValidationWork{root_index, std::vector<std::size_t>{}}};
  while (!stack.empty()) {
    ValidationWork work = std::move(stack.back());
    stack.pop_back();
    const std::size_t node_index = work.node_index;
    current_path = std::move(work.path);
    if (visited[node_index] != 0U) {
      return invalid(ValueInvariant::aliased_tuple_child, node_index);
    }
    visited[node_index] = 1U;

    const bool root = node_index == root_index;
    const ContainerKind container =
        root ? ContainerKind::tuple
             : value.tuple.nodes[node_index].container;
    if (container == ContainerKind::scalar) {
      const ValueNode &node = value.tuple.nodes[node_index];
      if (node.first_child != 0U || node.child_count != 0U ||
          node.tuple_reservation_index != 0U ||
          node.vector_payload_index != 0U) {
        return invalid(ValueInvariant::inactive_tuple_field, node_index);
      }
      const ValueValidationResult scalar_validation = validate_scalar(node.scalar);
      if (!scalar_validation.ok) {
        return invalid(scalar_validation.invariant, node_index);
      }
      continue;
    }
    if (container == ContainerKind::vector) {
      const ValueNode &node = value.tuple.nodes[node_index];
      if (!is_empty_scalar(node.scalar)) {
        return invalid(ValueInvariant::inactive_scalar_field, node_index);
      }
      if (node.first_child != 0U || node.child_count != 0U ||
          node.tuple_reservation_index != 0U) {
        return invalid(ValueInvariant::inactive_tuple_field, node_index);
      }
      if (node.vector_payload_index >= value.tuple.vector_payloads.size()) {
        return invalid(ValueInvariant::invalid_vector_payload_handle,
                       node_index);
      }
      if (payload_owners[node.vector_payload_index] != 0U) {
        return invalid(ValueInvariant::aliased_vector_payload, node_index);
      }
      payload_owners[node.vector_payload_index] = 1U;
      const void *storage = vector_storage_address(
          value.tuple.vector_payloads[node.vector_payload_index]);
      if (storage != nullptr) {
        for (std::size_t payload_index = 0U;
             payload_index < value.tuple.vector_payloads.size();
             ++payload_index) {
          if (payload_index == node.vector_payload_index ||
              payload_owners[payload_index] == 0U) {
            continue;
          }
          if (vector_storage_address(
                  value.tuple.vector_payloads[payload_index]) == storage) {
            return invalid(ValueInvariant::aliased_vector_payload, node_index);
          }
        }
        for (std::size_t reservation_index = 0U;
             reservation_index < reservation_owners.size();
             ++reservation_index) {
          if (reservation_owners[reservation_index] == 0U) {
            continue;
          }
          const TupleTableReservation &reservation =
              reservation_index == value.tuple.reservations.size()
                  ? value.tuple.root_reservation
                  : value.tuple.reservations[reservation_index];
          if (reservation.storage.get() == storage) {
            return invalid(ValueInvariant::aliased_vector_payload, node_index);
          }
        }
      }
      const ValueValidationResult vector_result =
          vector_validation(
              value.tuple.vector_payloads[node.vector_payload_index],
              node_index);
      if (!vector_result.ok) {
        return vector_result;
      }
      continue;
    }
    if (container != ContainerKind::tuple) {
      return invalid(ValueInvariant::unknown_container, node_index);
    }

    const ScalarValue &scalar = root ? value.scalar
                                     : value.tuple.nodes[node_index].scalar;
    if (!is_empty_scalar(scalar)) {
      return invalid(ValueInvariant::inactive_scalar_field, node_index);
    }
    if (!root && value.tuple.nodes[node_index].vector_payload_index != 0U) {
      return invalid(ValueInvariant::inactive_vector_payload, node_index);
    }
    const std::size_t first_child =
        root ? value.tuple.first_child
             : value.tuple.nodes[node_index].first_child;
    const std::size_t child_count =
        root ? value.tuple.child_count
             : value.tuple.nodes[node_index].child_count;
    if (first_child > value.tuple.child_indexes.size() ||
        child_count > value.tuple.child_indexes.size() - first_child) {
      return invalid(ValueInvariant::invalid_tuple_range, node_index,
                     first_child);
    }
    for (std::size_t offset = 0U; offset < child_count; ++offset) {
      const std::size_t edge_index = first_child + offset;
      if (edge_owners[edge_index] != 0U) {
        return invalid(ValueInvariant::overlapping_tuple_range, node_index,
                       edge_index);
      }
      edge_owners[edge_index] = 1U;
      const std::size_t child_index = value.tuple.child_indexes[edge_index];
      if (child_index >= node_count) {
        return invalid(ValueInvariant::invalid_tuple_child_index, node_index,
                       edge_index);
      }
      if (child_index >= node_index) {
        return invalid(ValueInvariant::non_postorder_tuple_child, node_index,
                       edge_index);
      }
      ++parent_count[child_index];
      if (parent_count[child_index] != 1U) {
        return invalid(ValueInvariant::aliased_tuple_child, child_index,
                       edge_index);
      }
    }

    const std::size_t reservation_index =
        root ? value.tuple.reservations.size()
             : value.tuple.nodes[node_index].tuple_reservation_index;
    if (reservation_index > value.tuple.reservations.size()) {
      return invalid(ValueInvariant::missing_tuple_reservation, node_index);
    }
    if (reservation_owners[reservation_index] != 0U) {
      return invalid(ValueInvariant::aliased_tuple_reservation, node_index);
    }
    reservation_owners[reservation_index] = 1U;
    const TupleTableReservation &reservation =
        root ? value.tuple.root_reservation
             : value.tuple.reservations[reservation_index];
    const void *reservation_storage = reservation.storage.get();
    if (reservation_storage != nullptr) {
      for (std::size_t prior_index = 0U;
           prior_index < reservation_owners.size(); ++prior_index) {
        if (prior_index == reservation_index ||
            reservation_owners[prior_index] == 0U) {
          continue;
        }
        const TupleTableReservation &prior =
            prior_index == value.tuple.reservations.size()
                ? value.tuple.root_reservation
                : value.tuple.reservations[prior_index];
        if (prior.storage.get() == reservation_storage) {
          return invalid(ValueInvariant::aliased_tuple_reservation,
                         node_index);
        }
      }
      for (std::size_t payload_index = 0U;
           payload_index < payload_owners.size(); ++payload_index) {
        if (payload_owners[payload_index] != 0U &&
            vector_storage_address(value.tuple.vector_payloads[payload_index]) ==
                reservation_storage) {
          return invalid(ValueInvariant::aliased_tuple_reservation,
                         node_index);
        }
      }
    }
    if (child_count > std::numeric_limits<std::size_t>::max() / 16U) {
      return invalid(ValueInvariant::invalid_tuple_reservation_count,
                     node_index);
    }
    const std::size_t expected_bytes = child_count * 16U;
    if (reservation.element_count != child_count ||
        reservation.canonical_bytes != expected_bytes ||
        (expected_bytes == 0U && reservation.storage.get() != nullptr) ||
        (expected_bytes != 0U && reservation.storage.get() == nullptr) ||
        (expected_bytes != 0U && reservation.accounting_active &&
         reservation.accounting_owner == nullptr) ||
        (expected_bytes != 0U && reservation.accounting_active &&
         !reservation.allocation_ordinal.has_value()) ||
        (expected_bytes == 0U && reservation.allocation_ordinal.has_value()) ||
        (expected_bytes == 0U &&
         reservation.accounting_owner != nullptr) ||
        (!reservation.accounting_active &&
         reservation.accounting_owner != nullptr)) {
      return invalid(ValueInvariant::invalid_tuple_reservation_count,
                     node_index);
    }
    for (std::size_t offset = child_count; offset > 0U; --offset) {
      const std::size_t child_offset = offset - 1U;
      std::vector<std::size_t> child_path = current_path;
      child_path.push_back(child_offset);
      stack.push_back(ValidationWork{
          value.tuple.child_indexes[first_child + child_offset],
          std::move(child_path)});
    }
  }

  for (std::size_t index = 0U; index < visited.size(); ++index) {
    if (visited[index] == 0U) {
      return invalid(ValueInvariant::orphan_value_node, index);
    }
  }
  for (std::size_t index = 0U; index < edge_owners.size(); ++index) {
    if (edge_owners[index] == 0U) {
      return invalid(ValueInvariant::orphan_tuple_edge, root_index, index);
    }
  }
  for (std::size_t index = 0U; index < payload_owners.size(); ++index) {
    if (payload_owners[index] == 0U) {
      return invalid(ValueInvariant::orphan_vector_payload_handle, root_index);
    }
  }
  for (std::size_t index = 0U; index < reservation_owners.size(); ++index) {
    if (reservation_owners[index] == 0U) {
      return invalid(ValueInvariant::orphan_tuple_reservation, root_index);
    }
  }
  return ValueValidationResult{true, ValueInvariant::none};
#if defined(__cpp_exceptions)
  } catch (const std::bad_alloc &) {
    ValueValidationResult result{false, ValueInvariant::none};
    result.resource_error = HostResourceErrorReason::allocation_unavailable;
    return result;
  } catch (const std::length_error &) {
    ValueValidationResult result{false, ValueInvariant::none};
    result.resource_error = HostResourceErrorReason::size_overflow;
    return result;
  }
#endif
}

ValueValidationResult value_element_type(const Value &value,
                                         ScalarType &element_type) {
  element_type = ScalarType::boolean;
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    return validation;
  }
  if (value.container == ContainerKind::tuple) {
    return ValueValidationResult{false, ValueInvariant::none,
                                 ValueAccessError::container_mismatch};
  }
  element_type = value.container == ContainerKind::scalar
                     ? value.scalar.type
                     : value.vector.element_type;
  return validation;
}

ValueValidationResult value_rank(const Value &value, std::size_t &rank) {
  rank = 0;
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    return validation;
  }
  if (value.container == ContainerKind::tuple) {
    return ValueValidationResult{false, ValueInvariant::none,
                                 ValueAccessError::container_mismatch};
  }
  rank = value.container == ContainerKind::scalar ? 0 : 1;
  return validation;
}

ValueValidationResult value_length(const Value &value, std::size_t &length) {
  length = 0;
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    return validation;
  }
  if (value.container == ContainerKind::tuple) {
    return ValueValidationResult{false, ValueInvariant::none,
                                 ValueAccessError::container_mismatch};
  }
  length = value.container == ContainerKind::scalar
               ? 1
               : active_vector_length(value.vector);
  return validation;
}

ScalarProjectionResult project_scalar(const Value &value, std::size_t index) {
  if (!validate_value(value).ok) {
    return ScalarProjectionResult{false, empty_scalar(),
                                  ValueAccessError::invalid_value};
  }
  if (value.container == ContainerKind::scalar) {
    return ScalarProjectionResult{true, value.scalar, ValueAccessError::none};
  }
  if (value.container == ContainerKind::tuple) {
    return ScalarProjectionResult{false, empty_scalar(),
                                  ValueAccessError::container_mismatch};
  }
  switch (value.vector.element_type) {
  case ScalarType::boolean:
    if (index >= value.vector.boolean_count) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::boolean,
                    value.vector.booleans.get()[index] != 0, 0,
                    0.0},
        ValueAccessError::none};
  case ScalarType::integer:
    if (index >= value.vector.integer_count) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::integer, false,
                    value.vector.integers.get()[index],
                    0.0},
        ValueAccessError::none};
  case ScalarType::double_precision:
    if (index >= value.vector.double_count) {
      return ScalarProjectionResult{false, empty_scalar(),
                                    ValueAccessError::index_out_of_bounds};
    }
    return ScalarProjectionResult{
        true,
        ScalarValue{ScalarType::double_precision, false, 0,
                    value.vector.doubles.get()[index]},
        ValueAccessError::none};
  }
  return ScalarProjectionResult{false, empty_scalar(),
                                ValueAccessError::invalid_value};
}

namespace {

ValueTypeResult value_type_resource_failure(HostResourceErrorReason reason) {
  ValueTypeResult result{false, TypeArena{{}, {}, 0U}, ValueInvariant::none,
                         ValueAccessError::none};
  result.resource_error = reason;
  return result;
}

ValueTypeResult type_for_value_node(
    const Value &value, std::size_t requested_root,
    HostAllocationFailureInjection &allocation_failure) {
  const std::size_t allocation_ordinal = allocation_failure.allocation_ordinal;
  if (allocation_ordinal == std::numeric_limits<std::size_t>::max() ||
      (allocation_failure.fail_at_allocation_ordinal.has_value() &&
       allocation_ordinal ==
           *allocation_failure.fail_at_allocation_ordinal)) {
    return value_type_resource_failure(
        HostResourceErrorReason::allocation_unavailable);
  }
  ++allocation_failure.allocation_ordinal;

  TypeArena type;
  if (value.container != ContainerKind::tuple) {
#if defined(__cpp_exceptions)
    try {
#endif
      type.nodes.reserve(1U);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc &) {
      return value_type_resource_failure(
          HostResourceErrorReason::allocation_unavailable);
    } catch (const std::length_error &) {
      return value_type_resource_failure(HostResourceErrorReason::size_overflow);
    }
#endif
    type.nodes.push_back(TypeNode{
        value.container == ContainerKind::scalar ? TypeKind::scalar
                                                 : TypeKind::vector,
        value.container == ContainerKind::scalar ? value.scalar.type
                                                 : value.vector.element_type,
        0U,
        0U,
    });
    type.root_index = 0U;
    return ValueTypeResult{true, std::move(type), ValueInvariant::none,
                           ValueAccessError::none};
  }

  const std::size_t logical_count = value.tuple.nodes.size() + 1U;
  std::vector<std::uint8_t> reachable;
  std::vector<std::size_t> stack;
  std::vector<std::size_t> index_map;
#if defined(__cpp_exceptions)
  try {
#endif
    reachable.reserve(logical_count);
    stack.reserve(logical_count);
    index_map.reserve(logical_count);
    type.nodes.reserve(logical_count);
    type.child_indexes.reserve(value.tuple.child_indexes.size());
#if defined(__cpp_exceptions)
  } catch (const std::bad_alloc &) {
    return value_type_resource_failure(
        HostResourceErrorReason::allocation_unavailable);
  } catch (const std::length_error &) {
    return value_type_resource_failure(HostResourceErrorReason::size_overflow);
  }
#endif
  reachable.resize(logical_count, 0U);
  index_map.resize(logical_count, 0U);
  stack.push_back(requested_root);
  while (!stack.empty()) {
    const std::size_t node_index = stack.back();
    stack.pop_back();
    if (reachable[node_index] != 0U) {
      continue;
    }
    reachable[node_index] = 1U;
    const bool root = node_index == value.tuple.root_index;
    const ContainerKind container =
        root ? ContainerKind::tuple
             : value.tuple.nodes[node_index].container;
    if (container != ContainerKind::tuple) {
      continue;
    }
    const std::size_t first_child =
        root ? value.tuple.first_child
             : value.tuple.nodes[node_index].first_child;
    const std::size_t child_count =
        root ? value.tuple.child_count
             : value.tuple.nodes[node_index].child_count;
    for (std::size_t offset = 0U; offset < child_count; ++offset) {
      stack.push_back(value.tuple.child_indexes[first_child + offset]);
    }
  }

  for (std::size_t node_index = 0U; node_index <= requested_root; ++node_index) {
    if (reachable[node_index] == 0U) {
      continue;
    }
    const bool root = node_index == value.tuple.root_index;
    const ContainerKind container =
        root ? ContainerKind::tuple
             : value.tuple.nodes[node_index].container;
    TypeNode node{TypeKind::scalar, ScalarType::boolean, 0U, 0U};
    if (container == ContainerKind::scalar) {
      node.scalar = value.tuple.nodes[node_index].scalar.type;
    } else if (container == ContainerKind::vector) {
      node.kind = TypeKind::vector;
      node.scalar = value
                        .tuple.vector_payloads[value.tuple.nodes[node_index]
                                                   .vector_payload_index]
                        .element_type;
    } else {
      node.kind = TypeKind::tuple;
      const std::size_t first_child =
          root ? value.tuple.first_child
               : value.tuple.nodes[node_index].first_child;
      const std::size_t child_count =
          root ? value.tuple.child_count
               : value.tuple.nodes[node_index].child_count;
      node.first_child = type.child_indexes.size();
      node.child_count = child_count;
      for (std::size_t offset = 0U; offset < child_count; ++offset) {
        type.child_indexes.push_back(index_map[
            value.tuple.child_indexes[first_child + offset]]);
      }
    }
    index_map[node_index] = type.nodes.size();
    type.nodes.push_back(node);
  }
  type.root_index = index_map[requested_root];
  return ValueTypeResult{true, std::move(type), ValueInvariant::none,
                         ValueAccessError::none};
}

bool append_vector(std::string &formatted, const VectorValue &vector) {
  const std::size_t length = active_vector_length(vector);
  formatted += '(';
  for (std::size_t index = 0U; index < length; ++index) {
    if (index != 0U) {
      formatted += ' ';
    }
    switch (vector.element_type) {
    case ScalarType::boolean:
      formatted += vector.booleans.get()[index] != 0U ? "true" : "false";
      break;
    case ScalarType::integer:
      if (!append_integer(formatted, vector.integers.get()[index])) {
        return false;
      }
      break;
    case ScalarType::double_precision:
      if (!append_double(formatted, vector.doubles.get()[index])) {
        return false;
      }
      break;
    }
  }
  formatted += ')';
  return true;
}

struct ValueFormatAction {
  enum class Kind {
    node,
    separator,
    close_tuple,
  } kind;
  std::size_t node_index;
};

} // namespace

ValueTypeResult value_type(const Value &value) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return value_type(value, allocation_failure);
}

ValueTypeResult value_type(
    const Value &value,
    HostAllocationFailureInjection &allocation_failure) {
  const ValueValidationResult validation =
      validate_value(value, allocation_failure);
  if (!validation.ok) {
    ValueTypeResult result{false, TypeArena{{}, {}, 0U},
                           validation.invariant,
                           validation.resource_error ==
                                   HostResourceErrorReason::none
                               ? ValueAccessError::invalid_value
                               : ValueAccessError::none};
    result.path = validation.path;
    result.node_index = validation.node_index;
    result.edge_index = validation.edge_index;
    result.resource_error = validation.resource_error;
    return result;
  }
  const std::size_t root = value.container == ContainerKind::tuple
                               ? value.tuple.root_index
                               : 0U;
  return type_for_value_node(value, root, allocation_failure);
}

ValueTypeResult value_type(BorrowedValueView view) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return value_type(view, allocation_failure);
}

ValueTypeResult value_type(
    BorrowedValueView view,
    HostAllocationFailureInjection &allocation_failure) {
  if (view.owner == nullptr) {
    return ValueTypeResult{false, TypeArena{{}, {}, 0U},
                           ValueInvariant::invalid_value_root,
                           ValueAccessError::invalid_value};
  }
  const ValueValidationResult validation =
      validate_value(*view.owner, allocation_failure);
  if (!validation.ok || view.owner->container != ContainerKind::tuple ||
      view.node_index >= view.owner->tuple.nodes.size()) {
    ValueTypeResult result{
        false, TypeArena{{}, {}, 0U},
        validation.ok ? ValueInvariant::invalid_value_root
                      : validation.invariant,
        validation.resource_error == HostResourceErrorReason::none
            ? ValueAccessError::invalid_value
            : ValueAccessError::none};
    if (!validation.ok) {
      result.path = validation.path;
      result.node_index = validation.node_index;
      result.edge_index = validation.edge_index;
      result.resource_error = validation.resource_error;
    } else {
      result.node_index = view.node_index;
    }
    return result;
  }
  return type_for_value_node(*view.owner, view.node_index,
                             allocation_failure);
}

ValueTupleArityResult value_tuple_arity(const Value &value) {
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    ValueTupleArityResult result{
        false, 0U, validation.invariant,
        validation.resource_error == HostResourceErrorReason::none
            ? ValueAccessError::invalid_value
            : ValueAccessError::none};
    result.path = validation.path;
    result.node_index = validation.node_index;
    result.edge_index = validation.edge_index;
    result.resource_error = validation.resource_error;
    return result;
  }
  if (value.container != ContainerKind::tuple) {
    return ValueTupleArityResult{false, 0U, ValueInvariant::none,
                                 ValueAccessError::container_mismatch};
  }
  return ValueTupleArityResult{true, value.tuple.child_count,
                               ValueInvariant::none, ValueAccessError::none};
}

ValueTupleElementResult value_tuple_element(const Value &value,
                                            std::size_t index) {
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    ValueTupleElementResult result{
        false, BorrowedValueView{nullptr, 0U}, validation.invariant,
        validation.resource_error == HostResourceErrorReason::none
            ? ValueAccessError::invalid_value
            : ValueAccessError::none};
    result.path = validation.path;
    result.node_index = validation.node_index;
    result.edge_index = validation.edge_index;
    result.resource_error = validation.resource_error;
    return result;
  }
  if (value.container != ContainerKind::tuple) {
    return ValueTupleElementResult{
        false, BorrowedValueView{nullptr, 0U}, ValueInvariant::none,
        ValueAccessError::container_mismatch};
  }
  if (index >= value.tuple.child_count) {
    return ValueTupleElementResult{
        false, BorrowedValueView{nullptr, 0U}, ValueInvariant::none,
        ValueAccessError::index_out_of_bounds};
  }
  return ValueTupleElementResult{
      true,
      BorrowedValueView{
          &value, value.tuple.child_indexes[value.tuple.first_child + index]},
      ValueInvariant::none, ValueAccessError::none};
}

ValueDestructionResult destroy_value(Value &value) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return destroy_value(value, allocation_failure);
}

ValueDestructionResult destroy_value(
    Value &value,
    HostAllocationFailureInjection &allocation_failure) {
  if (!value.claimed) {
    return ValueDestructionResult{true, ValueInvariant::none};
  }
  const ValueValidationResult validation =
      validate_value(value, allocation_failure);
  if (!validation.ok) {
    ValueDestructionResult result{false, validation.invariant};
    result.path = validation.path;
    result.node_index = validation.node_index;
    result.edge_index = validation.edge_index;
    result.resource_error = validation.resource_error;
    return result;
  }

  const auto release_vector_accounting = [](VectorValue &vector) {
    const void *storage = vector_storage_address(vector);
    if (vector.accounting_active && vector.accounting_owner != nullptr &&
        vector.canonical_bytes <= *vector.accounting_owner) {
      *vector.accounting_owner -= vector.canonical_bytes;
      observe_lifetime(vector.lifetime_observer,
                       ResourceLifetimeEventKind::logical_release,
                       ResourceStorageKind::vector_payload,
                       vector.allocation_ordinal, storage,
                       vector.canonical_bytes);
    }
    vector.accounting_active = false;
    vector.accounting_owner = nullptr;
    if (storage != nullptr) {
      observe_lifetime(vector.lifetime_observer,
                       ResourceLifetimeEventKind::physical_release,
                       ResourceStorageKind::vector_payload,
                       vector.allocation_ordinal, storage,
                       vector.canonical_bytes);
      reset_vector_storage(vector);
    }
    vector.allocation_ordinal = std::nullopt;
    vector.lifetime_observer = ResourceLifetimeObserver{};
  };
  const auto release_tuple_accounting = [](TupleTableReservation &reservation) {
    const void *storage = reservation.storage.get();
    if (reservation.accounting_active &&
        reservation.accounting_owner != nullptr &&
        reservation.canonical_bytes <= *reservation.accounting_owner) {
      *reservation.accounting_owner -= reservation.canonical_bytes;
      observe_lifetime(reservation.lifetime_observer,
                       ResourceLifetimeEventKind::logical_release,
                       ResourceStorageKind::tuple_table,
                       reservation.allocation_ordinal, storage,
                       reservation.canonical_bytes);
    }
    reservation.accounting_active = false;
    reservation.accounting_owner = nullptr;
    if (storage != nullptr) {
      observe_lifetime(reservation.lifetime_observer,
                       ResourceLifetimeEventKind::physical_release,
                       ResourceStorageKind::tuple_table,
                       reservation.allocation_ordinal, storage,
                       reservation.canonical_bytes);
      reservation.storage.reset();
    }
    reservation.allocation_ordinal = std::nullopt;
    reservation.lifetime_observer = ResourceLifetimeObserver{};
  };

  if (value.container == ContainerKind::vector) {
    release_vector_accounting(value.vector);
  } else if (value.container == ContainerKind::tuple) {
    const auto set_parent = [&value](std::size_t child_index,
                                     std::size_t parent_index) {
      ValueNode &child = value.tuple.nodes[child_index];
      if (child.container == ContainerKind::tuple) {
        child.vector_payload_index = parent_index;
      } else {
        child.first_child = parent_index;
      }
    };
    for (std::size_t node_index = 0U;
         node_index < value.tuple.nodes.size(); ++node_index) {
      const ValueNode &node = value.tuple.nodes[node_index];
      if (node.container != ContainerKind::tuple) {
        continue;
      }
      for (std::size_t offset = 0U; offset < node.child_count; ++offset) {
        set_parent(value.tuple.child_indexes[node.first_child + offset],
                   node_index);
      }
    }
    for (std::size_t offset = 0U; offset < value.tuple.child_count; ++offset) {
      set_parent(value.tuple.child_indexes[value.tuple.first_child + offset],
                 value.tuple.root_index);
    }

    std::size_t node_index = value.tuple.root_index;
    while (true) {
      const bool root = node_index == value.tuple.root_index;
      ValueNode *node = root ? nullptr : &value.tuple.nodes[node_index];
      const ContainerKind container =
          root ? ContainerKind::tuple : node->container;
      if (container == ContainerKind::tuple) {
        const std::size_t first_child =
            root ? value.tuple.first_child : node->first_child;
        std::size_t &remaining_children =
            root ? value.tuple.child_count : node->child_count;
        if (remaining_children != 0U) {
          --remaining_children;
          node_index =
              value.tuple.child_indexes[first_child + remaining_children];
          continue;
        }
        TupleTableReservation &reservation =
            root ? value.tuple.root_reservation
                 : value.tuple.reservations[node->tuple_reservation_index];
        release_tuple_accounting(reservation);
      } else if (container == ContainerKind::vector) {
        release_vector_accounting(
            value.tuple.vector_payloads[node->vector_payload_index]);
      }

      if (root) {
        break;
      }
      node_index = container == ContainerKind::tuple
                       ? node->vector_payload_index
                       : node->first_child;
    }
  }
  value = invalid_construction_value();
  return ValueDestructionResult{true, ValueInvariant::none};
}

ValueFormattingResult format_value(const Value &value) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return format_value(value, allocation_failure);
}

ValueFormattingResult format_value(
    const Value &value,
    HostAllocationFailureInjection &allocation_failure) {
  const ValueValidationResult validation =
      validate_value(value, allocation_failure);
  if (!validation.ok) {
    if (validation.resource_error != HostResourceErrorReason::none) {
      ValueFormattingResult result{false, {}, ValueInvariant::none,
                                   ValueFormatError::none};
      result.resource_error = validation.resource_error;
      return result;
    }
    ValueFormattingResult result{false, {}, validation.invariant,
                                 ValueFormatError::invalid_value};
    result.path = validation.path;
    result.node_index = validation.node_index;
    result.edge_index = validation.edge_index;
    result.resource_error = validation.resource_error;
    return result;
  }

  const std::size_t allocation_ordinal = allocation_failure.allocation_ordinal;
  if (allocation_ordinal == std::numeric_limits<std::size_t>::max() ||
      (allocation_failure.fail_at_allocation_ordinal.has_value() &&
       allocation_ordinal ==
           *allocation_failure.fail_at_allocation_ordinal)) {
    ValueFormattingResult result{false, {}, ValueInvariant::none,
                                 ValueFormatError::none};
    result.resource_error = HostResourceErrorReason::allocation_unavailable;
    return result;
  }
  ++allocation_failure.allocation_ordinal;
#if defined(__cpp_exceptions)
  try {
#endif

  std::string formatted;
  if (value.container == ContainerKind::scalar) {
    if (!append_scalar(formatted, value.scalar)) {
      return ValueFormattingResult{false, {}, ValueInvariant::none,
                                   ValueFormatError::conversion_failure};
    }
    return ValueFormattingResult{true, std::move(formatted),
                                 ValueInvariant::none, ValueFormatError::none};
  }
  if (value.container == ContainerKind::vector) {
    if (!append_vector(formatted, value.vector)) {
      return ValueFormattingResult{false, {}, ValueInvariant::none,
                                   ValueFormatError::conversion_failure};
    }
    return ValueFormattingResult{true, std::move(formatted),
                                 ValueInvariant::none, ValueFormatError::none};
  }

  std::vector<ValueFormatAction> stack;
  stack.push_back(ValueFormatAction{ValueFormatAction::Kind::node,
                                    value.tuple.root_index});
  while (!stack.empty()) {
    const ValueFormatAction action = stack.back();
    stack.pop_back();
    if (action.kind == ValueFormatAction::Kind::separator) {
      formatted += ' ';
      continue;
    }
    if (action.kind == ValueFormatAction::Kind::close_tuple) {
      formatted += ']';
      continue;
    }

    const bool root = action.node_index == value.tuple.root_index;
    const ContainerKind container =
        root ? ContainerKind::tuple
             : value.tuple.nodes[action.node_index].container;
    if (container == ContainerKind::scalar) {
      if (!append_scalar(formatted,
                         value.tuple.nodes[action.node_index].scalar)) {
        return ValueFormattingResult{false, {}, ValueInvariant::none,
                                     ValueFormatError::conversion_failure};
      }
      continue;
    }
    if (container == ContainerKind::vector) {
      if (!append_vector(
              formatted,
              value.tuple.vector_payloads[
                  value.tuple.nodes[action.node_index].vector_payload_index])) {
        return ValueFormattingResult{false, {}, ValueInvariant::none,
                                     ValueFormatError::conversion_failure};
      }
      continue;
    }

    formatted += '[';
    stack.push_back(
        ValueFormatAction{ValueFormatAction::Kind::close_tuple, 0U});
    const std::size_t first_child =
        root ? value.tuple.first_child
             : value.tuple.nodes[action.node_index].first_child;
    const std::size_t child_count =
        root ? value.tuple.child_count
             : value.tuple.nodes[action.node_index].child_count;
    for (std::size_t offset = child_count; offset > 0U; --offset) {
      const std::size_t child_offset = offset - 1U;
      stack.push_back(ValueFormatAction{
          ValueFormatAction::Kind::node,
          value.tuple.child_indexes[first_child + child_offset]});
      if (child_offset != 0U) {
        stack.push_back(
            ValueFormatAction{ValueFormatAction::Kind::separator, 0U});
      }
    }
  }
  return ValueFormattingResult{true, std::move(formatted), ValueInvariant::none,
                               ValueFormatError::none};
#if defined(__cpp_exceptions)
  } catch (const std::bad_alloc &) {
    ValueFormattingResult result{false, {}, ValueInvariant::none,
                                 ValueFormatError::none};
    result.resource_error = HostResourceErrorReason::allocation_unavailable;
    return result;
  } catch (const std::length_error &) {
    ValueFormattingResult result{false, {}, ValueInvariant::none,
                                 ValueFormatError::none};
    result.resource_error = HostResourceErrorReason::size_overflow;
    return result;
  }
#endif
}

namespace {

using ValueConstructionResult = VectorAllocationResult;

ValueConstructionResult
make_bool_vector(std::initializer_list<std::uint8_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  return copy_bool_vector(
      resources, std::span<const std::uint8_t>(values.begin(), values.size()),
      SourceLocation{0, 1, 1}, "value-construction");
}

ValueConstructionResult
make_int_vector(std::initializer_list<std::int64_t> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  return copy_int_vector(
      resources, std::span<const std::int64_t>(values.begin(), values.size()),
      SourceLocation{0, 1, 1}, "value-construction");
}

ValueConstructionResult make_double_vector(std::initializer_list<double> values) {
  EvaluationResources resources = make_trusted_local_resources({std::nullopt});
  return copy_double_vector(
      resources, std::span<const double>(values.begin(), values.size()),
      SourceLocation{0, 1, 1}, "value-construction");
}

[[maybe_unused]] std::string format_valid_test_value(const Value &value) {
  ValueFormattingResult result = format_value(value);
  CHECK(result.ok);
  return std::move(result.formatted);
}

[[maybe_unused]] ScalarType valid_element_type_for_test(const Value &value) {
  (void)value;
  ScalarType element_type = ScalarType::boolean;
  CHECK(value_element_type(value, element_type).ok);
  return element_type;
}

[[maybe_unused]] std::size_t valid_rank_for_test(const Value &value) {
  (void)value;
  std::size_t rank = 0;
  CHECK(value_rank(value, rank).ok);
  return rank;
}

[[maybe_unused]] std::size_t valid_length_for_test(const Value &value) {
  (void)value;
  std::size_t length = 0;
  CHECK(value_length(value, length).ok);
  return length;
}

} // namespace

TEST_CASE("typed scalar construction produces valid direct tagged values") {
  const Value boolean = make_bool_value(true);
  const Value integer = make_int_value(INT64_MIN);
  const Value double_precision = make_double_value(-0.0);

  CHECK(validate_value(boolean).ok);
  CHECK(boolean.container == ContainerKind::scalar);
  CHECK(boolean.scalar.type == ScalarType::boolean);
  CHECK(boolean.scalar.boolean);

  CHECK(validate_value(integer).ok);
  CHECK(integer.scalar.type == ScalarType::integer);
  CHECK(integer.scalar.integer == INT64_MIN);

  CHECK(validate_value(double_precision).ok);
  CHECK(double_precision.scalar.type == ScalarType::double_precision);
  CHECK(double_precision.scalar.double_precision == 0.0);
}

TEST_CASE("vectors keep one untagged typed payload and preserve empty types") {
  static_assert(std::is_same_v<decltype(VectorValue::booleans)::element_type,
                               std::uint8_t>);
  static_assert(std::is_same_v<decltype(VectorValue::integers)::element_type,
                               std::int64_t>);
  static_assert(
      std::is_same_v<decltype(VectorValue::doubles)::element_type, double>);

  const ValueConstructionResult booleans = make_bool_vector({});
  const ValueConstructionResult integers = make_int_vector({});
  const ValueConstructionResult doubles = make_double_vector({});

  REQUIRE(booleans.ok);
  REQUIRE(integers.ok);
  REQUIRE(doubles.ok);
  CHECK(booleans.value.vector.element_type == ScalarType::boolean);
  CHECK(integers.value.vector.element_type == ScalarType::integer);
  CHECK(doubles.value.vector.element_type == ScalarType::double_precision);
  CHECK(valid_length_for_test(booleans.value) == 0);
  CHECK(valid_length_for_test(integers.value) == 0);
  CHECK(valid_length_for_test(doubles.value) == 0);
  CHECK(format_valid_test_value(booleans.value) == "()");
  CHECK(format_valid_test_value(integers.value) == "()");
  CHECK(format_valid_test_value(doubles.value) == "()");
}

TEST_CASE("construction and validation reject invalid homogeneous payloads") {
  const ValueConstructionResult invalid_boolean = make_bool_vector({0, 2, 1});
  CHECK_FALSE(invalid_boolean.ok);
  CHECK(invalid_boolean.invariant == ValueInvariant::invalid_boolean_element);

  Value inactive = make_int_vector({1, 2}).value;
  Value unexpected_double = make_double_vector({2.0}).value;
  inactive.vector.doubles = std::move(unexpected_double.vector.doubles);
  inactive.vector.double_count = 1;
  const ValueValidationResult inactive_result = validate_value(inactive);
  CHECK_FALSE(inactive_result.ok);
  CHECK(inactive_result.invariant == ValueInvariant::inactive_vector_payload);

  Value inactive_tag = make_bool_value(false);
  inactive_tag.vector.element_type = ScalarType::integer;
  CHECK_FALSE(validate_value(inactive_tag).ok);

  Value inactive_scalar = make_int_vector({1}).value;
  inactive_scalar.scalar.integer = 9;
  const ValueValidationResult inactive_scalar_result =
      validate_value(inactive_scalar);
  CHECK_FALSE(inactive_scalar_result.ok);
  CHECK(inactive_scalar_result.invariant ==
        ValueInvariant::inactive_scalar_field);

  Value noncanonical = make_double_vector({1.0}).value;
  noncanonical.vector.doubles.get()[0] =
      std::bit_cast<double>(UINT64_C(0xfff8123456789abc));
  const ValueValidationResult nan_result = validate_value(noncanonical);
  CHECK_FALSE(nan_result.ok);
  CHECK(nan_result.invariant == ValueInvariant::noncanonical_nan);

  const ValueConstructionResult normalized = make_double_vector(
      {std::bit_cast<double>(UINT64_C(0xfff8123456789abc))});
  REQUIRE(normalized.ok);
  CHECK(std::bit_cast<std::uint64_t>(
            normalized.value.vector.doubles.get()[0]) ==
        canonical_nan_bits);
}

TEST_CASE("rank length and projection follow scalar and vector identity") {
  const Value scalar = make_int_value(42);
  const Value vector = make_int_vector({4, 5, 6}).value;

  CHECK(valid_rank_for_test(scalar) == 0);
  CHECK(valid_length_for_test(scalar) == 1);
  CHECK(valid_element_type_for_test(scalar) == ScalarType::integer);
  const ScalarProjectionResult broadcast = project_scalar(scalar, 12);
  REQUIRE(broadcast.ok);
  CHECK(broadcast.value.integer == 42);

  CHECK(valid_rank_for_test(vector) == 1);
  CHECK(valid_length_for_test(vector) == 3);
  CHECK(valid_element_type_for_test(vector) == ScalarType::integer);
  const ScalarProjectionResult projected = project_scalar(vector, 1);
  REQUIRE(projected.ok);
  CHECK(projected.value.type == ScalarType::integer);
  CHECK(projected.value.integer == 5);
  const ScalarProjectionResult outside = project_scalar(vector, 3);
  CHECK_FALSE(outside.ok);
  CHECK(outside.error == ValueAccessError::index_out_of_bounds);
}

TEST_CASE("canonical scalar and vector formatting is byte exact") {
  CHECK(format_valid_test_value(make_bool_value(false)) == "false");
  CHECK(format_valid_test_value(make_bool_value(true)) == "true");
  CHECK(format_valid_test_value(make_int_value(INT64_MIN)) ==
        "-9223372036854775808");
  CHECK(format_valid_test_value(make_int_value(INT64_MAX)) ==
        "9223372036854775807");
  CHECK(format_valid_test_value(make_double_value(1.0)) == "1.0");
  CHECK(format_valid_test_value(make_double_value(-42.0)) == "-42.0");
  CHECK(format_valid_test_value(make_double_value(1.0e20)) == "1e20");
  CHECK(format_valid_test_value(make_double_value(1.0e-7)) == "1e-7");
  CHECK(format_valid_test_value(make_double_value(-0.0)) == "-0.0");
  CHECK(format_valid_test_value(
            make_double_value(std::numeric_limits<double>::infinity())) ==
        "inf");
  CHECK(format_valid_test_value(make_double_value(
            -std::numeric_limits<double>::infinity())) == "-inf");
  CHECK(format_valid_test_value(make_double_value(
            std::numeric_limits<double>::quiet_NaN())) == "nan");

  CHECK(format_valid_test_value(make_bool_vector({0, 1, 0}).value) ==
        "(false true false)");
  CHECK(format_valid_test_value(make_int_vector({1, -2, 3}).value) ==
        "(1 -2 3)");
  CHECK(format_valid_test_value(make_double_vector({1.0, 2.5, 3.0}).value) ==
        "(1.0 2.5 3.0)");
  CHECK(format_valid_test_value(
            make_double_vector({std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(), -0.0})
                .value) == "(nan inf -inf -0.0)");
}

TEST_CASE("binary64 formatting round trips boundaries and normalizes spelling") {
  struct FormatCase {
    std::uint64_t bits;
    const char *expected;
  };
  const std::array<FormatCase, 7> cases{{
      {UINT64_C(0x0000000000000001), "5e-324"},
      {UINT64_C(0x000fffffffffffff), "2.225073858507201e-308"},
      {UINT64_C(0x0010000000000000), "2.2250738585072014e-308"},
      {UINT64_C(0x7fefffffffffffff), "1.7976931348623157e308"},
      {UINT64_C(0x3ff0000000000000), "1.0"},
      {UINT64_C(0x8000000000000000), "-0.0"},
      {UINT64_C(0x7ff0000000000000), "inf"},
  }};
  for (const FormatCase &format_case : cases) {
    CAPTURE(format_case.expected);
    const double value = std::bit_cast<double>(format_case.bits);
    const std::string formatted =
        format_valid_test_value(make_double_value(value));
    CHECK(formatted == format_case.expected);
    if ((format_case.bits & binary64_exponent_mask) !=
        binary64_exponent_mask) {
      char *round_trip_end = nullptr;
      const double parsed = std::strtod(formatted.c_str(), &round_trip_end);
      CHECK(round_trip_end == formatted.c_str() + formatted.size());
      CHECK(std::bit_cast<std::uint64_t>(parsed) == format_case.bits);
    }
  }

  const Value normalized = make_double_value(
      std::bit_cast<double>(UINT64_C(0xfff0000000000001)));
  CHECK(std::bit_cast<std::uint64_t>(normalized.scalar.double_precision) ==
        UINT64_C(0x7ff8000000000000));
  CHECK(format_valid_test_value(normalized) == "nan");
}

TEST_CASE("public value consumers reject malformed plain records") {
  Value unknown_container = make_int_vector({1}).value;
  unknown_container.container = static_cast<ContainerKind>(99);
  Value unknown_scalar = make_bool_value(false);
  unknown_scalar.scalar.type = static_cast<ScalarType>(99);
  Value inactive_payload = make_int_vector({1}).value;
  Value unexpected_payload = make_double_vector({2.0}).value;
  inactive_payload.vector.doubles =
      std::move(unexpected_payload.vector.doubles);
  inactive_payload.vector.double_count = 1;
  Value invalid_boolean = make_bool_vector({0, 1}).value;
  invalid_boolean.vector.booleans.get()[1] = 2;
  Value noncanonical_nan = make_double_vector({1.0}).value;
  noncanonical_nan.vector.doubles.get()[0] =
      std::bit_cast<double>(UINT64_C(0xfff8123456789abc));

  const std::array<const Value *, 5> invalid_values{{
      &unknown_container,
      &unknown_scalar,
      &inactive_payload,
      &invalid_boolean,
      &noncanonical_nan,
  }};
  for (const Value *value : invalid_values) {
    CAPTURE(validate_value(*value).invariant);
    CHECK_FALSE(validate_value(*value).ok);

    ScalarType element_type = ScalarType::double_precision;
    const ValueValidationResult element_result =
        value_element_type(*value, element_type);
    CHECK_FALSE(element_result.ok);
    CHECK(element_result.invariant == validate_value(*value).invariant);
    CHECK(element_type == ScalarType::boolean);

    std::size_t rank = 99;
    const ValueValidationResult rank_result = value_rank(*value, rank);
    CHECK_FALSE(rank_result.ok);
    CHECK(rank_result.invariant == validate_value(*value).invariant);
    CHECK(rank == 0);

    std::size_t length = 99;
    const ValueValidationResult length_result = value_length(*value, length);
    CHECK_FALSE(length_result.ok);
    CHECK(length_result.invariant == validate_value(*value).invariant);
    CHECK(length == 0);

    const ScalarProjectionResult projection = project_scalar(*value, 0);
    CHECK_FALSE(projection.ok);
    CHECK(projection.error == ValueAccessError::invalid_value);

    const ValueFormattingResult formatting = format_value(*value);
    CHECK_FALSE(formatting.ok);
    CHECK(formatting.formatted.empty());
    CHECK(formatting.invariant == validate_value(*value).invariant);
    CHECK(formatting.error == ValueFormatError::invalid_value);
  }
}

TEST_CASE("explicit destruction releases owned payload and leaves an empty owner") {
  Value value = make_double_vector({1.0, 2.0, 3.0}).value;
  REQUIRE(value.vector.double_count == 3);

  destroy_value(value);

  CHECK_FALSE(validate_value(value).ok);
  CHECK_FALSE(value.claimed);
  CHECK(value.container == ContainerKind::scalar);
  CHECK(value.scalar.type == ScalarType::boolean);
  CHECK_FALSE(value.scalar.boolean);
  CHECK(value.vector.booleans.get() == nullptr);
  CHECK(value.vector.boolean_count == 0);
  CHECK(value.vector.integers.get() == nullptr);
  CHECK(value.vector.integer_count == 0);
  CHECK(value.vector.doubles.get() == nullptr);
  CHECK(value.vector.double_count == 0);
}

} // namespace bennu
