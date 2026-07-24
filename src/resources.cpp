#include "bennu/resources.hpp"

#include "doctest/doctest.h"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace bennu {

struct EvaluationResourceState {
  std::size_t reference_count;
  bool context_active;
  std::uint64_t token;
  EvaluationResourceState *registry_next;
  std::mutex transaction_mutex;
  ExecutionProfile profile;
  ResourceLimits limits;
  HostResourceErrorReason creation_error;
  AllocationFailureInjection allocation_failure;
  std::size_t live_evaluation_bytes;
  std::size_t work_units;
  std::size_t reservation_ordinal;
  ResourceLifetimeObserver lifetime_observer;
};

namespace {

constexpr std::size_t resource_registry_bucket_count = 257U;
std::array<EvaluationResourceState *, resource_registry_bucket_count>
    resource_registry{};
std::mutex resource_registry_mutex;
std::atomic<std::uint64_t> next_resource_token{1U};

std::size_t resource_registry_bucket(std::uint64_t token) {
  return static_cast<std::size_t>(
      token % resource_registry_bucket_count);
}

EvaluationResourceState *find_resource_state_locked(
    EvaluationResourceOwner owner) {
  if (owner.token == 0U) {
    return nullptr;
  }
  EvaluationResourceState *state =
      resource_registry[resource_registry_bucket(owner.token)];
  while (state != nullptr && state->token != owner.token) {
    state = state->registry_next;
  }
  return state;
}

void erase_and_destroy_resource_state_locked(
    EvaluationResourceState *state) {
  EvaluationResourceState **link =
      &resource_registry[resource_registry_bucket(state->token)];
  while (*link != nullptr && *link != state) {
    link = &(*link)->registry_next;
  }
  if (*link == state) {
    *link = state->registry_next;
    std::destroy_at(state);
    std::free(state);
  }
}

bool record_matches_state(const EvaluationResources &resources,
                          const EvaluationResourceState &state) {
  return resources.owner.token == state.token &&
         resources.state_handle.token == state.token &&
         &resources.profile == &state.profile &&
         &resources.limits == &state.limits &&
         &resources.creation_error == &state.creation_error &&
         &resources.allocation_failure == &state.allocation_failure &&
         &resources.live_evaluation_bytes ==
             &state.live_evaluation_bytes &&
         &resources.work_units == &state.work_units &&
         &resources.reservation_ordinal == &state.reservation_ordinal &&
         &resources.lifetime_observer == &state.lifetime_observer;
}

EvaluationResourceState *validated_resource_state(
    const EvaluationResources &resources) {
  const std::lock_guard<std::mutex> registry_lock(
      resource_registry_mutex);
  EvaluationResourceState *state =
      find_resource_state_locked(resources.owner);
  if (state == nullptr || !state->context_active ||
      !record_matches_state(resources, *state)) {
    return nullptr;
  }
  return state;
}

EvaluationResourceState *acquire_accounting_owner(
    EvaluationResourceOwner owner) {
  const std::lock_guard<std::mutex> registry_lock(
      resource_registry_mutex);
  EvaluationResourceState *state = find_resource_state_locked(owner);
  if (state == nullptr) {
    return nullptr;
  }
  ++state->reference_count;
  return state;
}

void release_acquired_resource_state(EvaluationResourceState *state) {
  if (state == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> registry_lock(
      resource_registry_mutex);
  --state->reference_count;
  if (state->reference_count == 0U) {
    erase_and_destroy_resource_state_locked(state);
  }
}

bool retain_accounting_owner(EvaluationResourceOwner owner) {
  const std::lock_guard<std::mutex> registry_lock(
      resource_registry_mutex);
  EvaluationResourceState *state = find_resource_state_locked(owner);
  if (state == nullptr) {
    return false;
  }
  ++state->reference_count;
  return true;
}

void release_accounting_owner(EvaluationResourceOwner owner) {
  const std::lock_guard<std::mutex> registry_lock(
      resource_registry_mutex);
  EvaluationResourceState *state = find_resource_state_locked(owner);
  if (state == nullptr) {
    return;
  }
  --state->reference_count;
  if (state->reference_count == 0U) {
    erase_and_destroy_resource_state_locked(state);
  }
}

bool same_owner(EvaluationResourceOwner left,
                EvaluationResourceOwner right) {
  return left.token == right.token;
}

} // namespace

namespace detail {

bool resource_state_owner_is_live(EvaluationResourceOwner owner) {
  const std::lock_guard<std::mutex> registry_lock(
      resource_registry_mutex);
  return find_resource_state_locked(owner) != nullptr;
}

bool refund_resource_state_bytes(EvaluationResourceOwner owner,
                                 std::size_t byte_count) {
  EvaluationResourceState *state = acquire_accounting_owner(owner);
  if (state == nullptr) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(state->transaction_mutex);
  if (byte_count > state->live_evaluation_bytes) {
    release_acquired_resource_state(state);
    return false;
  }
  state->live_evaluation_bytes -= byte_count;
  release_acquired_resource_state(state);
  return true;
}

void release_resource_state_owner(EvaluationResourceOwner owner) {
  release_accounting_owner(owner);
}

} // namespace detail

namespace {

ExecutionProfile failed_profile{ExecutionProfile::trusted_local_v1};
ResourceLimits failed_limits{};
HostResourceErrorReason failed_creation_error{
    HostResourceErrorReason::allocation_unavailable};
AllocationFailureInjection failed_allocation_failure{std::nullopt};
std::size_t failed_live_evaluation_bytes{0U};
std::size_t failed_work_units{0U};
std::size_t failed_reservation_ordinal{0U};
ResourceLifetimeObserver failed_lifetime_observer{};

EvaluationResources make_resource_record(
    ExecutionProfile profile, ResourceLimits limits,
    AllocationFailureInjection allocation_failure,
    std::size_t live_evaluation_bytes, std::size_t work_units,
    std::size_t reservation_ordinal) {
  void *allocation = std::malloc(sizeof(EvaluationResourceState));
  if (allocation == nullptr) {
    return EvaluationResources{
        EvaluationResourceOwner{}, EvaluationResourceStateHandle{},
        failed_profile, failed_limits, failed_creation_error,
        failed_allocation_failure, failed_live_evaluation_bytes,
        failed_work_units, failed_reservation_ordinal,
        failed_lifetime_observer};
  }
  auto *state = new (allocation) EvaluationResourceState{};
  const std::uint64_t token =
      next_resource_token.fetch_add(1U, std::memory_order_relaxed);
  if (token == 0U) {
    std::destroy_at(state);
    std::free(state);
    return EvaluationResources{
        EvaluationResourceOwner{}, EvaluationResourceStateHandle{},
        failed_profile, failed_limits, failed_creation_error,
        failed_allocation_failure, failed_live_evaluation_bytes,
        failed_work_units, failed_reservation_ordinal,
        failed_lifetime_observer};
  }
  state->reference_count = 1U;
  state->context_active = true;
  state->token = token;
  state->registry_next = nullptr;
  state->profile = profile;
  state->limits = std::move(limits);
  state->creation_error = HostResourceErrorReason::none;
  state->allocation_failure = allocation_failure;
  state->live_evaluation_bytes = live_evaluation_bytes;
  state->work_units = work_units;
  state->reservation_ordinal = reservation_ordinal;
  state->lifetime_observer = ResourceLifetimeObserver{};
  {
    const std::lock_guard<std::mutex> registry_lock(
        resource_registry_mutex);
    EvaluationResourceState *&head =
        resource_registry[resource_registry_bucket(token)];
    state->registry_next = head;
    head = state;
  }
  return EvaluationResources{
      EvaluationResourceOwner{token}, EvaluationResourceStateHandle{token},
      state->profile, state->limits, state->creation_error,
      state->allocation_failure, state->live_evaluation_bytes,
      state->work_units, state->reservation_ordinal,
      state->lifetime_observer};
}

constexpr std::size_t bool_payload_width = 1;
constexpr std::size_t int_payload_width = 8;
constexpr std::size_t double_payload_width = 8;
static_assert(sizeof(std::uint8_t) == bool_payload_width);
static_assert(sizeof(std::int64_t) == int_payload_width);
static_assert(sizeof(double) == double_payload_width);

Error no_error() {
  return make_error(ErrorKind::none, SourceLocation{0, 1, 1});
}

Value empty_vector(ScalarType element_type) {
  return Value{
      ContainerKind::vector,
      ScalarValue{ScalarType::boolean, false, 0, 0.0},
      VectorValue{element_type,
                  {nullptr, &release_host_buffer<std::uint8_t>},
                  0,
                  {nullptr, &release_host_buffer<std::int64_t>},
                  0,
                  {nullptr, &release_host_buffer<double>},
                  0},
  };
}

WorkspaceReservation empty_workspace() {
  return WorkspaceReservation{
      {nullptr, &release_host_buffer<std::byte>}, 0};
}

std::optional<std::size_t> element_width(ScalarType element_type) {
  switch (element_type) {
  case ScalarType::boolean:
    return bool_payload_width;
  case ScalarType::integer:
    return int_payload_width;
  case ScalarType::double_precision:
    return double_payload_width;
  }
  return std::nullopt;
}

const void *vector_storage(const VectorValue &vector) {
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

bool tuple_contains_physical_storage(const TupleValue &tuple,
                                     const void *storage) {
  if (storage == nullptr) {
    return false;
  }
  if (tuple.nodes.storage.get() == storage ||
      tuple.child_indexes.storage.get() == storage ||
      tuple.vector_payloads.storage.get() == storage ||
      tuple.reservations.storage.get() == storage ||
      tuple.root_reservation.storage.get() == storage) {
    return true;
  }
  for (const VectorValue &payload :
       host_array_span(tuple.vector_payloads)) {
    if (vector_storage(payload) == storage) {
      return true;
    }
  }
  for (const TupleTableReservation &reservation :
       host_array_span(tuple.reservations)) {
    if (reservation.storage.get() == storage) {
      return true;
    }
  }
  return false;
}

bool value_contains_physical_storage(const Value &value,
                                     const void *storage) {
  if (value.container == ContainerKind::vector &&
      vector_storage(value.vector) == storage && storage != nullptr) {
    return true;
  }
  return value.container == ContainerKind::tuple &&
         tuple_contains_physical_storage(value.tuple, storage);
}

bool tuple_storage_overlaps_value(const TupleValue &tuple,
                                  const Value &other) {
  const std::array<const void *, 5U> table_storage{{
      tuple.nodes.storage.get(),
      tuple.child_indexes.storage.get(),
      tuple.vector_payloads.storage.get(),
      tuple.reservations.storage.get(),
      tuple.root_reservation.storage.get(),
  }};
  for (const void *storage : table_storage) {
    if (storage != nullptr &&
        value_contains_physical_storage(other, storage)) {
      return true;
    }
  }
  for (const VectorValue &payload :
       host_array_span(tuple.vector_payloads)) {
    if (value_contains_physical_storage(other,
                                        vector_storage(payload))) {
      return true;
    }
  }
  for (const TupleTableReservation &reservation :
       host_array_span(tuple.reservations)) {
    if (value_contains_physical_storage(other,
                                        reservation.storage.get())) {
      return true;
    }
  }
  return false;
}

bool physical_storage_overlaps(const Value &left,
                               const Value &right) {
  if (left.container == ContainerKind::vector) {
    return value_contains_physical_storage(
        right, vector_storage(left.vector));
  }
  return left.container == ContainerKind::tuple &&
         tuple_storage_overlaps_value(left.tuple, right);
}

void observe_lifetime(const ResourceLifetimeObserver &observer,
                      ResourceLifetimeEventKind kind,
                      ResourceStorageKind storage_kind,
                      std::optional<std::size_t> allocation_ordinal,
                      const void *storage, std::size_t canonical_bytes);

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

void release_proven_vector(VectorValue &vector) {
  const void *storage = vector_storage(vector);
  if (vector.accounting_active &&
      vector.accounting_owner.token != 0U &&
      detail::refund_resource_state_bytes(
          vector.accounting_owner, vector.canonical_bytes)) {
    observe_lifetime(vector.lifetime_observer,
                     ResourceLifetimeEventKind::logical_release,
                     ResourceStorageKind::vector_payload,
                     vector.allocation_ordinal, storage,
                     vector.canonical_bytes);
  }
  const EvaluationResourceOwner accounting_owner =
      vector.accounting_owner;
  vector.accounting_active = false;
  vector.accounting_owner = EvaluationResourceOwner{};
  detail::release_resource_state_owner(accounting_owner);
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
}

void release_proven_tuple_reservation(
    TupleTableReservation &reservation) {
  const void *storage = reservation.storage.get();
  if (reservation.accounting_active &&
      reservation.accounting_owner.token != 0U &&
      detail::refund_resource_state_bytes(
          reservation.accounting_owner,
          reservation.canonical_bytes)) {
    observe_lifetime(reservation.lifetime_observer,
                     ResourceLifetimeEventKind::logical_release,
                     ResourceStorageKind::tuple_table,
                     reservation.allocation_ordinal, storage,
                     reservation.canonical_bytes);
  }
  const EvaluationResourceOwner accounting_owner =
      reservation.accounting_owner;
  reservation.accounting_active = false;
  reservation.accounting_owner = EvaluationResourceOwner{};
  detail::release_resource_state_owner(accounting_owner);
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
}

void destroy_proven_value(Value &value) {
  if (!value.claimed) {
    return;
  }
  if (value.container == ContainerKind::vector) {
    release_proven_vector(value.vector);
  } else if (value.container == ContainerKind::tuple) {
    const auto set_parent = [&value](std::size_t child_index,
                                     std::size_t parent_index) {
      ValueNode &child = value.tuple.nodes.storage.get()[child_index];
      if (child.container == ContainerKind::tuple) {
        child.vector_payload_index = parent_index;
      } else {
        child.first_child = parent_index;
      }
    };
    for (std::size_t node_index = 0U;
         node_index < value.tuple.nodes.size; ++node_index) {
      const ValueNode &node =
          value.tuple.nodes.storage.get()[node_index];
      if (node.container != ContainerKind::tuple) {
        continue;
      }
      for (std::size_t offset = 0U; offset < node.child_count; ++offset) {
        set_parent(value.tuple.child_indexes.storage.get()[
                       node.first_child + offset],
                   node_index);
      }
    }
    for (std::size_t offset = 0U;
         offset < value.tuple.child_count; ++offset) {
      set_parent(value.tuple.child_indexes.storage.get()[
                     value.tuple.first_child + offset],
                 value.tuple.root_index);
    }

    std::size_t node_index = value.tuple.root_index;
    while (true) {
      const bool root = node_index == value.tuple.root_index;
      ValueNode *node =
          root ? nullptr
               : &value.tuple.nodes.storage.get()[node_index];
      const ContainerKind container =
          root ? ContainerKind::tuple : node->container;
      if (container == ContainerKind::tuple) {
        const std::size_t first_child =
            root ? value.tuple.first_child : node->first_child;
        std::size_t &remaining_children =
            root ? value.tuple.child_count : node->child_count;
        if (remaining_children != 0U) {
          --remaining_children;
          node_index = value.tuple.child_indexes.storage.get()[
              first_child + remaining_children];
          continue;
        }
        release_proven_tuple_reservation(
            root ? value.tuple.root_reservation
                 : value.tuple.reservations.storage.get()[
                       node->tuple_reservation_index]);
      } else if (container == ContainerKind::vector) {
        release_proven_vector(
            value.tuple.vector_payloads.storage.get()[
                node->vector_payload_index]);
      }
      if (root) {
        break;
      }
      node_index = container == ContainerKind::tuple
                       ? node->vector_payload_index
                       : node->first_child;
    }
  }
  value = make_int_value(0);
  value.claimed = false;
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

void observe_lifetime_unlocked(
    std::unique_lock<std::mutex> &transaction,
    const ResourceLifetimeObserver &observer,
    ResourceLifetimeEventKind kind,
    ResourceStorageKind storage_kind,
    std::optional<std::size_t> allocation_ordinal,
    const void *storage, std::size_t canonical_bytes) {
  const bool relock = transaction.owns_lock();
  if (relock) {
    transaction.unlock();
  }
  observe_lifetime(observer, kind, storage_kind, allocation_ordinal,
                   storage, canonical_bytes);
  if (relock) {
    transaction.lock();
  }
}

Error make_resource_failure(
    const EvaluationResources &resources, ResourceErrorReason reason,
    SourceLocation location, std::string_view producer_name,
    std::optional<std::size_t> requested_elements,
    std::optional<std::size_t> requested_bytes,
    std::optional<ResourceLimitKind> limit_kind,
    std::optional<std::size_t> configured_limit,
    std::optional<std::size_t> usage_before,
    std::optional<std::size_t> refused_charge,
    std::optional<std::size_t> allocation_ordinal = std::nullopt) {
  Error error = make_error(ErrorKind::resource_error, location);
  if (!producer_name.empty()) {
    error.primitive =
        make_primitive_error_context(producer_name, std::nullopt);
  }
  const EvaluationResourceState *state =
      validated_resource_state(resources);
  error.resource = ResourceErrorContext{
      reason,
      requested_elements,
      requested_bytes,
      state == nullptr ? std::string_view{}
                       : execution_profile_name(state->profile),
      limit_kind,
      configured_limit,
      usage_before,
      refused_charge,
      allocation_ordinal,
  };
  return error;
}

Error make_unsupported_tuple_profile(
    const EvaluationResources &resources, SourceLocation location,
    std::string_view producer_name) {
  Error error = make_error(ErrorKind::profile_error, location);
  if (!producer_name.empty()) {
    error.primitive =
        make_primitive_error_context(producer_name, std::nullopt);
  }
  const EvaluationResourceState *state =
      validated_resource_state(resources);
  error.profile = ProfileErrorContext{
      ProfileErrorReason::unsupported_value_kind,
      state == nullptr ? std::string_view{}
                       : execution_profile_name(state->profile),
      TypeKind::tuple};
  return error;
}

std::optional<Error>
validate_profile_configuration(const EvaluationResources &resources,
                               SourceLocation location,
                               std::string_view producer_name) {
  EvaluationResourceState *state =
      validated_resource_state(resources);
  if (state == nullptr ||
      state->creation_error != HostResourceErrorReason::none) {
    return make_resource_failure(
        resources,
        ResourceErrorReason::allocation_unavailable, location,
        producer_name, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt);
  }
  const bool has_configured_limit =
      resources.limits.max_vector_bytes.has_value() ||
      resources.limits.max_live_evaluation_bytes.has_value() ||
      resources.limits.max_work_units.has_value() ||
      resources.limits.max_tuple_table_bytes.has_value();
  std::string_view message;
  switch (resources.profile) {
  case ExecutionProfile::trusted_local_v1:
    if (!has_configured_limit) {
      return std::nullopt;
    }
    message = "trusted-local-v1 requires every resource limit to be omitted";
    break;
  case ExecutionProfile::bounded_v1:
    if (has_configured_limit &&
        !resources.limits.max_tuple_table_bytes.has_value()) {
      return std::nullopt;
    }
    message = resources.limits.max_tuple_table_bytes.has_value()
                  ? "bounded-v1 does not support max_tuple_table_bytes"
                  : "bounded-v1 requires at least one configured resource limit";
    break;
  case ExecutionProfile::trusted_local_v2:
    if (!has_configured_limit) {
      return std::nullopt;
    }
    message = "trusted-local-v2 requires every resource limit to be omitted";
    break;
  case ExecutionProfile::bounded_v2:
    if (has_configured_limit) {
      return std::nullopt;
    }
    message = "bounded-v2 requires at least one configured resource limit";
    break;
  default:
    message = "execution profile tag is unknown";
    break;
  }

  Error error =
      make_error(ErrorKind::invalid_execution_profile, location);
  error.static_message = message;
  if (!producer_name.empty()) {
    error.primitive =
        make_primitive_error_context(producer_name, std::nullopt);
  }
  return error;
}

struct AdmissionResult {
  bool ok;
  std::size_t live_after;
  std::size_t work_after;
  Error error;
};

AdmissionResult preflight(
    EvaluationResources &resources,
    std::optional<std::size_t> vector_bytes,
    std::size_t live_charge, std::size_t work_charge,
    SourceLocation location, std::string_view producer_name,
    std::optional<std::size_t> requested_elements,
    std::optional<std::size_t> requested_bytes) {
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (live_charge > maximum - resources.live_evaluation_bytes ||
      work_charge > maximum - resources.work_units) {
    return AdmissionResult{
        false,
        resources.live_evaluation_bytes,
        resources.work_units,
        make_resource_failure(resources, ResourceErrorReason::size_overflow,
                              location, producer_name, requested_elements,
                              requested_bytes, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt),
    };
  }
  const std::size_t live_after = resources.live_evaluation_bytes + live_charge;
  const std::size_t work_after = resources.work_units + work_charge;

  if (vector_bytes.has_value() &&
      resources.limits.max_vector_bytes.has_value() &&
      *vector_bytes > *resources.limits.max_vector_bytes) {
    return AdmissionResult{
        false,
        resources.live_evaluation_bytes,
        resources.work_units,
        make_resource_failure(
            resources, ResourceErrorReason::profile_limit, location,
            producer_name, requested_elements, requested_bytes,
            ResourceLimitKind::max_vector_bytes,
            resources.limits.max_vector_bytes, 0, *vector_bytes),
    };
  }
  if (resources.limits.max_live_evaluation_bytes.has_value() &&
      live_after > *resources.limits.max_live_evaluation_bytes) {
    return AdmissionResult{
        false,
        resources.live_evaluation_bytes,
        resources.work_units,
        make_resource_failure(
            resources, ResourceErrorReason::profile_limit, location,
            producer_name, requested_elements, requested_bytes,
            ResourceLimitKind::max_live_evaluation_bytes,
            resources.limits.max_live_evaluation_bytes,
            resources.live_evaluation_bytes, live_charge),
    };
  }
  if (resources.limits.max_work_units.has_value() &&
      work_after > *resources.limits.max_work_units) {
    return AdmissionResult{
        false,
        resources.live_evaluation_bytes,
        resources.work_units,
        make_resource_failure(resources, ResourceErrorReason::profile_limit,
                              location, producer_name, requested_elements,
                              requested_bytes,
                              ResourceLimitKind::max_work_units,
                              resources.limits.max_work_units,
                              resources.work_units, work_charge),
    };
  }
  return AdmissionResult{true, live_after, work_after, no_error()};
}

void commit_admission(EvaluationResources &resources,
                      const AdmissionResult &admission) {
  resources.live_evaluation_bytes = admission.live_after;
  resources.work_units = admission.work_after;
}

VectorAllocationResult allocation_failure(Value, Error error) {
  return VectorAllocationResult{false, make_int_value(0), ValueInvariant::none,
                                std::move(error)};
}

} // namespace

void release_evaluation_resources(EvaluationResources &resources) {
  const std::lock_guard<std::mutex> registry_lock(
      resource_registry_mutex);
  EvaluationResourceState *state =
      find_resource_state_locked(resources.owner);
  if (state != nullptr && state->context_active &&
      record_matches_state(resources, *state)) {
    state->context_active = false;
    --state->reference_count;
    if (state->reference_count == 0U) {
      erase_and_destroy_resource_state_locked(state);
    }
  }
  resources.owner = EvaluationResourceOwner{};
  resources.state_handle = EvaluationResourceStateHandle{};
}

EvaluationResources
move_evaluation_resources(EvaluationResources &resources) {
  EvaluationResources moved = resources;
  resources.owner = EvaluationResourceOwner{};
  resources.state_handle = EvaluationResourceStateHandle{};
  return moved;
}

std::string_view execution_profile_name(ExecutionProfile profile) {
  switch (profile) {
  case ExecutionProfile::trusted_local_v1:
    return "trusted-local-v1";
  case ExecutionProfile::bounded_v1:
    return "bounded-v1";
  case ExecutionProfile::trusted_local_v2:
    return "trusted-local-v2";
  case ExecutionProfile::bounded_v2:
    return "bounded-v2";
  }
  return "";
}

EvaluationResources make_trusted_local_resources(
    AllocationFailureInjection allocation_failure) {
  return make_resource_record(
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      allocation_failure,
      0,
      0,
      0);
}

EvaluationResources make_bounded_resources(
    ResourceLimits limits, AllocationFailureInjection allocation_failure) {
  return make_resource_record(ExecutionProfile::bounded_v1,
                              std::move(limits), allocation_failure,
                              0, 0, 0);
}

EvaluationResources make_trusted_local_v2_resources(
    AllocationFailureInjection allocation_failure) {
  return make_resource_record(
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      allocation_failure,
      0U,
      0U,
      0U);
}

EvaluationResources make_bounded_v2_resources(
    ResourceLimits limits, AllocationFailureInjection allocation_failure) {
  return make_resource_record(ExecutionProfile::bounded_v2,
                              std::move(limits), allocation_failure,
                              0U, 0U, 0U);
}

EvaluationResources make_evaluation_resources(
    ExecutionProfile profile, ResourceLimits limits,
    AllocationFailureInjection allocation_failure,
    std::size_t live_evaluation_bytes, std::size_t work_units,
    std::size_t reservation_ordinal) {
  return make_resource_record(profile, std::move(limits),
                              allocation_failure,
                              live_evaluation_bytes, work_units,
                              reservation_ordinal);
}

VectorAllocationResult allocate_vector(EvaluationResources &resources,
                                       ScalarType element_type,
                                       std::uint64_t requested_element_count,
                                       std::size_t work_units,
                                       SourceLocation location,
                                       std::string_view producer_name) {
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }
  std::optional<Error> profile_error =
      validate_profile_configuration(resources, location, producer_name);
  if (profile_error.has_value()) {
    return allocation_failure(make_int_value(0), std::move(*profile_error));
  }
  Value candidate = empty_vector(element_type);
  const std::optional<std::size_t> width = element_width(element_type);
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (requested_element_count > maximum) {
    return allocation_failure(
        std::move(candidate),
        make_resource_failure(resources, ResourceErrorReason::size_overflow,
                              location, producer_name, std::nullopt,
                              std::nullopt, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt));
  }
  const std::size_t element_count =
      static_cast<std::size_t>(requested_element_count);
  if (!width.has_value() || element_count > maximum / *width) {
    return allocation_failure(
        std::move(candidate),
        make_resource_failure(resources, ResourceErrorReason::size_overflow,
                              location, producer_name, element_count,
                              std::nullopt, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt));
  }
  const std::size_t byte_count = element_count * *width;
  AdmissionResult admission =
      preflight(resources, byte_count, byte_count, work_units, location,
                producer_name, element_count, byte_count);
  if (!admission.ok) {
    return allocation_failure(std::move(candidate), std::move(admission.error));
  }

  if (byte_count == 0) {
    commit_admission(resources, admission);
    return VectorAllocationResult{true, std::move(candidate),
                                  ValueInvariant::none, no_error()};
  }

  if (resources.reservation_ordinal ==
      std::numeric_limits<std::size_t>::max()) {
    return allocation_failure(
        std::move(candidate),
        make_resource_failure(resources, ResourceErrorReason::size_overflow,
                              location, producer_name, element_count,
                              byte_count, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt));
  }
  const std::size_t ordinal = resources.reservation_ordinal;
  ++resources.reservation_ordinal;
  if (resources.allocation_failure.fail_at_reservation_ordinal.has_value() &&
      ordinal ==
          *resources.allocation_failure.fail_at_reservation_ordinal) {
    return allocation_failure(
        std::move(candidate),
        make_resource_failure(
            resources, ResourceErrorReason::allocation_unavailable, location,
            producer_name, element_count, byte_count, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, ordinal));
  }

  HostResourceErrorReason host_allocation =
      HostResourceErrorReason::none;
  switch (element_type) {
  case ScalarType::boolean:
    host_allocation = allocate_host_buffer(
        candidate.vector.booleans, element_count);
    break;
  case ScalarType::integer:
    host_allocation = allocate_host_buffer(
        candidate.vector.integers, element_count);
    break;
  case ScalarType::double_precision:
    host_allocation = allocate_host_buffer(
        candidate.vector.doubles, element_count);
    break;
  }
  if (host_allocation != HostResourceErrorReason::none) {
    return allocation_failure(
        std::move(candidate),
        make_resource_failure(
            resources,
            host_allocation == HostResourceErrorReason::size_overflow
                ? ResourceErrorReason::size_overflow
                : ResourceErrorReason::allocation_unavailable,
            location,
            producer_name, element_count, byte_count, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, ordinal));
  }
  void *storage = const_cast<void *>(vector_storage(candidate.vector));
  std::memset(storage, 0, byte_count);
  switch (element_type) {
  case ScalarType::boolean:
    candidate.vector.boolean_count = element_count;
    break;
  case ScalarType::integer:
    candidate.vector.integer_count = element_count;
    break;
  case ScalarType::double_precision:
    candidate.vector.double_count = element_count;
    break;
  }
  candidate.vector.canonical_bytes = byte_count;
  candidate.vector.accounting_active = true;
  candidate.vector.accounting_owner = resources.owner;
  if (!retain_accounting_owner(resources.owner)) {
    reset_vector_storage(candidate.vector);
    return allocation_failure(
        std::move(candidate),
        make_resource_failure(
            resources, ResourceErrorReason::allocation_unavailable,
            location, producer_name, element_count, byte_count,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            ordinal));
  }
  candidate.vector.allocation_ordinal = ordinal;
  candidate.vector.lifetime_observer = resources.lifetime_observer;
  commit_admission(resources, admission);
  if (transaction.owns_lock()) {
    transaction.unlock();
  }
  observe_lifetime(resources.lifetime_observer,
                   ResourceLifetimeEventKind::admitted,
                   ResourceStorageKind::vector_payload, ordinal,
                   vector_storage(candidate.vector), byte_count);
  return VectorAllocationResult{true, std::move(candidate), ValueInvariant::none,
                                no_error()};
}

VectorAllocationResult copy_bool_vector(
    EvaluationResources &resources, std::span<const std::uint8_t> values,
    SourceLocation location, std::string_view producer_name) {
  for (const std::uint8_t value : values) {
    if (value > 1U) {
      return VectorAllocationResult{false, make_int_value(0),
                                    ValueInvariant::invalid_boolean_element,
                                    no_error()};
    }
  }
  VectorAllocationResult result = allocate_vector(
      resources, ScalarType::boolean,
      static_cast<std::uint64_t>(values.size()), 0, location, producer_name);
  if (result.ok && !values.empty()) {
    std::memcpy(result.value.vector.booleans.get(), values.data(),
                values.size());
  }
  return result;
}

VectorAllocationResult copy_int_vector(
    EvaluationResources &resources, std::span<const std::int64_t> values,
    SourceLocation location, std::string_view producer_name) {
  VectorAllocationResult result = allocate_vector(
      resources, ScalarType::integer,
      static_cast<std::uint64_t>(values.size()), 0, location, producer_name);
  if (result.ok && !values.empty()) {
    std::memcpy(result.value.vector.integers.get(), values.data(),
                values.size_bytes());
  }
  return result;
}

VectorAllocationResult copy_double_vector(
    EvaluationResources &resources, std::span<const double> values,
    SourceLocation location, std::string_view producer_name) {
  VectorAllocationResult result = allocate_vector(
      resources, ScalarType::double_precision,
      static_cast<std::uint64_t>(values.size()), 0, location, producer_name);
  if (result.ok) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      result.value.vector.doubles.get()[index] =
          make_double_value(values[index]).scalar.double_precision;
    }
  }
  return result;
}

WorkspaceReservationResult reserve_workspace(
    EvaluationResources &resources, std::size_t byte_count,
    std::size_t work_units, SourceLocation location,
    std::string_view producer_name) {
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }
  std::optional<Error> profile_error =
      validate_profile_configuration(resources, location, producer_name);
  if (profile_error.has_value()) {
    return WorkspaceReservationResult{false, empty_workspace(),
                                      std::move(*profile_error)};
  }
  AdmissionResult admission =
      preflight(resources, std::nullopt, byte_count, work_units, location,
                producer_name, std::nullopt, byte_count);
  if (!admission.ok) {
    return WorkspaceReservationResult{false, empty_workspace(),
                                      std::move(admission.error)};
  }
  if (byte_count == 0) {
    commit_admission(resources, admission);
    return WorkspaceReservationResult{true, empty_workspace(), no_error()};
  }

  if (resources.reservation_ordinal ==
      std::numeric_limits<std::size_t>::max()) {
    return WorkspaceReservationResult{
        false,
        empty_workspace(),
        make_resource_failure(resources, ResourceErrorReason::size_overflow,
                              location, producer_name, std::nullopt, byte_count,
                              std::nullopt, std::nullopt, std::nullopt,
                              std::nullopt),
    };
  }
  const std::size_t ordinal = resources.reservation_ordinal;
  ++resources.reservation_ordinal;
  if (resources.allocation_failure.fail_at_reservation_ordinal.has_value() &&
      ordinal ==
          *resources.allocation_failure.fail_at_reservation_ordinal) {
    return WorkspaceReservationResult{
        false,
        empty_workspace(),
        make_resource_failure(
            resources, ResourceErrorReason::allocation_unavailable, location,
            producer_name, std::nullopt, byte_count, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, ordinal),
    };
  }
  WorkspaceStorage storage{nullptr, &release_host_buffer<std::byte>};
  const HostResourceErrorReason host_allocation =
      allocate_host_buffer(storage, byte_count);
  if (host_allocation != HostResourceErrorReason::none) {
    return WorkspaceReservationResult{
        false,
        empty_workspace(),
        make_resource_failure(
            resources,
            host_allocation == HostResourceErrorReason::size_overflow
                ? ResourceErrorReason::size_overflow
                : ResourceErrorReason::allocation_unavailable,
            location,
            producer_name, std::nullopt, byte_count, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, ordinal),
    };
  }
  commit_admission(resources, admission);
  return WorkspaceReservationResult{
      true,
      WorkspaceReservation{std::move(storage), byte_count},
      no_error(),
  };
}

WorkChargeResult charge_work(EvaluationResources &resources,
                             std::size_t work_units,
                             SourceLocation location,
                             std::string_view producer_name) {
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }
  std::optional<Error> profile_error =
      validate_profile_configuration(resources, location, producer_name);
  if (profile_error.has_value()) {
    return WorkChargeResult{false, std::move(*profile_error)};
  }
  AdmissionResult admission =
      preflight(resources, std::nullopt, 0, work_units, location,
                producer_name, std::nullopt, std::nullopt);
  if (!admission.ok) {
    return WorkChargeResult{false, std::move(admission.error)};
  }
  commit_admission(resources, admission);
  return WorkChargeResult{true, no_error()};
}

TupleReservationResult reserve_tuple_table(
    EvaluationResources &resources, std::size_t element_count,
    SourceLocation location, std::string_view producer_name) {
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }
  std::optional<Error> profile_error =
      validate_profile_configuration(resources, location, producer_name);
  if (profile_error.has_value()) {
    return TupleReservationResult{false, TupleTableReservation{},
                                  std::move(*profile_error)};
  }
  if (resources.profile == ExecutionProfile::trusted_local_v1 ||
      resources.profile == ExecutionProfile::bounded_v1) {
    return TupleReservationResult{
        false, TupleTableReservation{},
        make_unsupported_tuple_profile(resources, location, producer_name)};
  }

  constexpr std::size_t tuple_element_slot_bytes = 16U;
  if (element_count > std::numeric_limits<std::size_t>::max() /
                          tuple_element_slot_bytes) {
    return TupleReservationResult{
        false,
        TupleTableReservation{},
        make_resource_failure(resources, ResourceErrorReason::size_overflow,
                              location, producer_name, element_count,
                              std::nullopt, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt),
    };
  }
  const std::size_t byte_count = element_count * tuple_element_slot_bytes;
  if (resources.limits.max_tuple_table_bytes.has_value() &&
      byte_count > *resources.limits.max_tuple_table_bytes) {
    return TupleReservationResult{
        false,
        TupleTableReservation{},
        make_resource_failure(
            resources, ResourceErrorReason::profile_limit, location,
            producer_name, element_count, byte_count,
            ResourceLimitKind::max_tuple_table_bytes,
            resources.limits.max_tuple_table_bytes, 0U, byte_count),
    };
  }

  AdmissionResult admission =
      preflight(resources, std::nullopt, byte_count, 0U, location,
                producer_name, element_count, byte_count);
  if (!admission.ok) {
    return TupleReservationResult{false, TupleTableReservation{},
                                  std::move(admission.error)};
  }
  if (byte_count == 0U) {
    commit_admission(resources, admission);
    TupleTableReservation reservation;
    reservation.accounting_active = true;
    return TupleReservationResult{true, std::move(reservation), no_error()};
  }

  if (resources.reservation_ordinal ==
      std::numeric_limits<std::size_t>::max()) {
    return TupleReservationResult{
        false,
        TupleTableReservation{},
        make_resource_failure(resources, ResourceErrorReason::size_overflow,
                              location, producer_name, element_count,
                              byte_count, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt),
    };
  }
  const std::size_t ordinal = resources.reservation_ordinal;
  ++resources.reservation_ordinal;
  if (resources.allocation_failure.fail_at_reservation_ordinal.has_value() &&
      ordinal ==
          *resources.allocation_failure.fail_at_reservation_ordinal) {
    Error error = make_resource_failure(
        resources, ResourceErrorReason::allocation_unavailable, location,
        producer_name, element_count, byte_count, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt);
    error.resource->allocation_ordinal = ordinal;
    return TupleReservationResult{false, TupleTableReservation{},
                                  std::move(error)};
  }
  TupleTableStorage storage{nullptr, &release_host_buffer<std::byte>};
  const HostResourceErrorReason host_allocation =
      allocate_host_buffer(storage, byte_count);
  if (host_allocation != HostResourceErrorReason::none) {
    Error error = make_resource_failure(
        resources,
        host_allocation == HostResourceErrorReason::size_overflow
            ? ResourceErrorReason::size_overflow
            : ResourceErrorReason::allocation_unavailable,
        location,
        producer_name, element_count, byte_count, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt);
    error.resource->allocation_ordinal = ordinal;
    return TupleReservationResult{false, TupleTableReservation{},
                                  std::move(error)};
  }
  std::memset(storage.get(), 0, byte_count);
  commit_admission(resources, admission);
  TupleTableReservation reservation;
  reservation.storage = std::move(storage);
  reservation.element_count = element_count;
  reservation.canonical_bytes = byte_count;
  reservation.accounting_active = true;
  reservation.accounting_owner = resources.owner;
  if (!retain_accounting_owner(resources.owner)) {
    reservation.storage.reset();
    return TupleReservationResult{
        false, TupleTableReservation{},
        make_resource_failure(
            resources, ResourceErrorReason::allocation_unavailable,
            location, producer_name, element_count, byte_count,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            ordinal)};
  }
  reservation.allocation_ordinal = ordinal;
  reservation.lifetime_observer = resources.lifetime_observer;
  if (transaction.owns_lock()) {
    transaction.unlock();
  }
  observe_lifetime(resources.lifetime_observer,
                   ResourceLifetimeEventKind::admitted,
                   ResourceStorageKind::tuple_table, ordinal,
                   reservation.storage.get(), byte_count);
  return TupleReservationResult{true, std::move(reservation), no_error()};
}

ValueReleaseResult release_tuple_table(
    EvaluationResources &resources, TupleTableReservation &reservation) {
  if (reservation.storage == nullptr &&
      reservation.element_count == 0U &&
      reservation.canonical_bytes == 0U &&
      !reservation.accounting_active &&
      reservation.accounting_owner.token == 0U &&
      !reservation.allocation_ordinal.has_value()) {
    return ValueReleaseResult{true, ValueInvariant::none};
  }
  if (reservation.element_count >
      std::numeric_limits<std::size_t>::max() / 16U) {
    return ValueReleaseResult{
        false, ValueInvariant::invalid_tuple_reservation_count};
  }
  const std::size_t expected_bytes = reservation.element_count * 16U;
  const bool metadata_valid =
      reservation.canonical_bytes == expected_bytes &&
      host_buffer_valid(reservation.storage, expected_bytes) &&
      ((expected_bytes == 0U && reservation.storage == nullptr &&
        reservation.accounting_active &&
        reservation.accounting_owner.token == 0U &&
        !reservation.allocation_ordinal.has_value()) ||
       (expected_bytes != 0U && reservation.storage != nullptr &&
        reservation.accounting_active &&
        reservation.accounting_owner.token != 0U &&
        reservation.allocation_ordinal.has_value()));
  if (!metadata_valid) {
    return ValueReleaseResult{
        false, ValueInvariant::invalid_tuple_reservation_count};
  }
  if (reservation.accounting_owner.token != 0U &&
      !same_owner(reservation.accounting_owner, resources.owner)) {
    return ValueReleaseResult{false, ValueInvariant::none,
                              ValueReleaseError::resource_context_mismatch};
  }
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  if (resource_state == nullptr) {
    return ValueReleaseResult{false, ValueInvariant::none,
                              ValueReleaseError::resource_context_mismatch};
  }
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }
  const void *storage = reservation.storage.get();
  const ResourceLifetimeObserver observer =
      reservation.lifetime_observer;
  const std::optional<std::size_t> ordinal =
      reservation.allocation_ordinal;
  const std::size_t canonical_bytes = reservation.canonical_bytes;
  bool logical_release = false;
  if (reservation.canonical_bytes <= resources.live_evaluation_bytes) {
    resources.live_evaluation_bytes -= reservation.canonical_bytes;
    logical_release = reservation.canonical_bytes != 0U;
  }
  const EvaluationResourceOwner accounting_owner =
      reservation.accounting_owner;
  reservation.accounting_active = false;
  reservation.accounting_owner = EvaluationResourceOwner{};
  detail::release_resource_state_owner(accounting_owner);
  const bool physical_release = storage != nullptr;
  if (storage != nullptr) {
    reservation.storage.reset();
  }
  reservation.element_count = 0U;
  reservation.canonical_bytes = 0U;
  reservation.allocation_ordinal = std::nullopt;
  reservation.lifetime_observer = ResourceLifetimeObserver{};
  if (transaction.owns_lock()) {
    transaction.unlock();
  }
  if (logical_release) {
    observe_lifetime(observer, ResourceLifetimeEventKind::logical_release,
                     ResourceStorageKind::tuple_table, ordinal, storage,
                     canonical_bytes);
  }
  if (physical_release) {
    observe_lifetime(observer, ResourceLifetimeEventKind::physical_release,
                     ResourceStorageKind::tuple_table, ordinal, storage,
                     canonical_bytes);
  }
  return ValueReleaseResult{true, ValueInvariant::none};
}

TupleConstructionResult make_tuple_value(
    EvaluationResources &resources, std::span<Value> elements,
    SourceLocation location, std::string_view producer_name) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return make_tuple_value(resources, elements, location, producer_name,
                          allocation_failure);
}

TupleConstructionResult make_tuple_value(
    EvaluationResources &resources, std::span<Value> elements,
    SourceLocation location, std::string_view producer_name,
    HostAllocationFailureInjection &allocation_failure) {
  std::optional<Error> configuration_error =
      validate_profile_configuration(resources, location, producer_name);
  if (configuration_error.has_value()) {
    return TupleConstructionResult{false, make_int_value(0),
                                   ValueInvariant::none,
                                   std::move(*configuration_error)};
  }
  if (resources.profile == ExecutionProfile::trusted_local_v1 ||
      resources.profile == ExecutionProfile::bounded_v1) {
    return TupleConstructionResult{false, make_int_value(0),
                                   ValueInvariant::none,
                                   make_unsupported_tuple_profile(
                                       resources, location, producer_name)};
  }

  std::size_t node_count = elements.size();
  std::size_t edge_count = elements.size();
  std::size_t payload_count = 0U;
  std::size_t reservation_count = 0U;
  for (std::size_t element_index = 0U;
       element_index < elements.size(); ++element_index) {
    const Value &element = elements[element_index];
    const ValueValidationResult validation =
        validate_value(element, allocation_failure);
    if (!validation.ok) {
      if (validation.resource_error != HostResourceErrorReason::none) {
        const ResourceErrorReason reason =
            validation.resource_error == HostResourceErrorReason::size_overflow
                ? ResourceErrorReason::size_overflow
                : ResourceErrorReason::allocation_unavailable;
        Error error = make_resource_failure(
            resources, reason, location, producer_name, elements.size(),
            std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt);
        return TupleConstructionResult{false, make_int_value(0),
                                       ValueInvariant::none,
                                       std::move(error)};
      }
      return TupleConstructionResult{false, make_int_value(0),
                                     validation.invariant, no_error()};
    }
    for (std::size_t prior_index = 0U;
         prior_index < element_index; ++prior_index) {
      if (physical_storage_overlaps(element, elements[prior_index])) {
        return TupleConstructionResult{
            false, make_int_value(0),
            ValueInvariant::aliased_vector_payload, no_error()};
      }
    }
    if (element.container == ContainerKind::vector) {
      ++payload_count;
      continue;
    }
    if (element.container != ContainerKind::tuple) {
      continue;
    }
    if (element.tuple.nodes.size >
            std::numeric_limits<std::size_t>::max() - node_count ||
        element.tuple.child_indexes.size >
            std::numeric_limits<std::size_t>::max() - edge_count ||
        element.tuple.vector_payloads.size >
            std::numeric_limits<std::size_t>::max() - payload_count ||
        element.tuple.reservations.size + 1U >
            std::numeric_limits<std::size_t>::max() - reservation_count) {
      Error error = make_resource_failure(
          resources, ResourceErrorReason::size_overflow, location,
          producer_name, elements.size(), std::nullopt, std::nullopt,
          std::nullopt, std::nullopt, std::nullopt);
      return TupleConstructionResult{false, make_int_value(0),
                                     ValueInvariant::none, std::move(error)};
    }
    node_count += element.tuple.nodes.size;
    edge_count += element.tuple.child_indexes.size;
    payload_count += element.tuple.vector_payloads.size;
    reservation_count += element.tuple.reservations.size + 1U;
  }

  TupleReservationResult reserved = reserve_tuple_table(
      resources, elements.size(), location, producer_name);
  if (!reserved.ok) {
    return TupleConstructionResult{false, make_int_value(0),
                                   ValueInvariant::none,
                                   std::move(reserved.error)};
  }

  const std::size_t reserved_bytes =
      reserved.reservation.canonical_bytes;
  const std::optional<std::size_t> tuple_ordinal =
      reserved.reservation.allocation_ordinal;
  const auto rollback_reservation = [&resources, &reserved]() {
    (void)release_tuple_table(resources, reserved.reservation);
  };
  const auto metadata_failure =
      [&](HostResourceErrorReason host_reason) {
    rollback_reservation();
    const ResourceErrorReason reason =
        host_reason == HostResourceErrorReason::size_overflow
            ? ResourceErrorReason::size_overflow
            : ResourceErrorReason::allocation_unavailable;
    Error error = make_resource_failure(
        resources, reason, location,
        producer_name, elements.size(), reserved_bytes,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        elements.empty() ? std::nullopt : tuple_ordinal);
    return TupleConstructionResult{false, make_int_value(0),
                                   ValueInvariant::none, std::move(error)};
  };

  Value result{ContainerKind::tuple,
               ScalarValue{ScalarType::boolean, false, 0, 0.0},
               VectorValue{ScalarType::boolean,
                           {nullptr, &release_host_buffer<std::uint8_t>},
                           0U,
                           {nullptr, &release_host_buffer<std::int64_t>},
                           0U,
                           {nullptr, &release_host_buffer<double>},
                           0U}};
  HostArray<std::size_t> roots;
  if (node_count != 0U || edge_count != 0U || payload_count != 0U ||
      reservation_count != 0U || !elements.empty()) {
    HostResourceErrorReason allocated =
        begin_host_allocation(allocation_failure);
    if (allocated == HostResourceErrorReason::none) {
      allocated = allocate_host_array(
          result.tuple.nodes, node_count,
          allocation_failure.max_container_elements);
    }
    if (allocated == HostResourceErrorReason::none) {
      allocated = allocate_host_array(
          result.tuple.child_indexes, edge_count,
          allocation_failure.max_container_elements);
    }
    if (allocated == HostResourceErrorReason::none) {
      allocated = allocate_host_array(
          result.tuple.vector_payloads, payload_count,
          allocation_failure.max_container_elements);
    }
    if (allocated == HostResourceErrorReason::none) {
      allocated = allocate_host_array(
          result.tuple.reservations, reservation_count,
          allocation_failure.max_container_elements);
    }
    if (allocated == HostResourceErrorReason::none) {
      allocated = allocate_host_array(
          roots, elements.size(),
          allocation_failure.max_container_elements);
    }
    if (allocated != HostResourceErrorReason::none) {
      return metadata_failure(allocated);
    }
    if (!host_array_has_capacity(result.tuple.nodes, node_count) ||
        !host_array_has_capacity(result.tuple.child_indexes, edge_count) ||
        !host_array_has_capacity(result.tuple.vector_payloads,
                                 payload_count) ||
        !host_array_has_capacity(result.tuple.reservations,
                                 reservation_count) ||
        !host_array_has_capacity(roots, elements.size())) {
      return metadata_failure(HostResourceErrorReason::size_overflow);
    }
  }

  for (Value &element : elements) {
    Value moved = move_value(element);
    if (moved.container != ContainerKind::tuple) {
      HostResourceErrorReason pushed =
          host_array_push(roots, result.tuple.nodes.size);
      if (pushed != HostResourceErrorReason::none) {
        return metadata_failure(pushed);
      }
      std::size_t payload_index = 0U;
      if (moved.container == ContainerKind::vector) {
        payload_index = result.tuple.vector_payloads.size;
        pushed = host_array_push(result.tuple.vector_payloads,
                                 std::move(moved.vector));
        if (pushed != HostResourceErrorReason::none) {
          return metadata_failure(pushed);
        }
      }
      pushed = host_array_push(
          result.tuple.nodes,
          ValueNode{moved.container, moved.scalar, 0U, 0U, 0U,
                    payload_index});
      if (pushed != HostResourceErrorReason::none) {
        return metadata_failure(pushed);
      }
      moved.claimed = false;
      continue;
    }

    const std::size_t node_offset = result.tuple.nodes.size;
    const std::size_t edge_offset = result.tuple.child_indexes.size;
    const std::size_t payload_offset =
        result.tuple.vector_payloads.size;
    const std::size_t reservation_offset =
        result.tuple.reservations.size;
    for (ValueNode &source_node :
         host_array_span(moved.tuple.nodes)) {
      if (source_node.container == ContainerKind::tuple) {
        source_node.first_child += edge_offset;
        source_node.tuple_reservation_index += reservation_offset;
      } else if (source_node.container == ContainerKind::vector) {
        source_node.vector_payload_index += payload_offset;
      }
      const HostResourceErrorReason pushed =
          host_array_push(result.tuple.nodes, std::move(source_node));
      if (pushed != HostResourceErrorReason::none) {
        return metadata_failure(pushed);
      }
    }
    for (const std::size_t child_index :
         host_array_span(moved.tuple.child_indexes)) {
      const HostResourceErrorReason pushed = host_array_push(
          result.tuple.child_indexes, child_index + node_offset);
      if (pushed != HostResourceErrorReason::none) {
        return metadata_failure(pushed);
      }
    }
    for (VectorValue &payload :
         host_array_span(moved.tuple.vector_payloads)) {
      const HostResourceErrorReason pushed = host_array_push(
          result.tuple.vector_payloads, std::move(payload));
      if (pushed != HostResourceErrorReason::none) {
        return metadata_failure(pushed);
      }
    }
    for (TupleTableReservation &reservation :
         host_array_span(moved.tuple.reservations)) {
      const HostResourceErrorReason pushed = host_array_push(
          result.tuple.reservations, std::move(reservation));
      if (pushed != HostResourceErrorReason::none) {
        return metadata_failure(pushed);
      }
    }
    const std::size_t root_reservation_index =
        result.tuple.reservations.size;
    HostResourceErrorReason pushed = host_array_push(
        result.tuple.reservations,
        std::move(moved.tuple.root_reservation));
    if (pushed != HostResourceErrorReason::none) {
      return metadata_failure(pushed);
    }
    pushed = host_array_push(roots, result.tuple.nodes.size);
    if (pushed != HostResourceErrorReason::none) {
      return metadata_failure(pushed);
    }
    pushed = host_array_push(result.tuple.nodes, ValueNode{
        ContainerKind::tuple,
        moved.scalar,
        moved.tuple.first_child + edge_offset,
        moved.tuple.child_count,
        root_reservation_index,
        0U,
    });
    if (pushed != HostResourceErrorReason::none) {
      return metadata_failure(pushed);
    }
    moved.claimed = false;
  }

  result.tuple.first_child = result.tuple.child_indexes.size;
  for (const std::size_t root : host_array_span(roots)) {
    const HostResourceErrorReason pushed =
        host_array_push(result.tuple.child_indexes, root);
    if (pushed != HostResourceErrorReason::none) {
      return metadata_failure(pushed);
    }
  }
  result.tuple.child_count = roots.size;
  result.tuple.root_index = result.tuple.nodes.size;
  result.tuple.root_reservation = std::move(reserved.reservation);
  return TupleConstructionResult{true, std::move(result), ValueInvariant::none,
                                 no_error()};
}

TupleConstructionResult execute_tuple_construction(
    EvaluationResources &resources, std::span<Value> element_storage,
    TupleElementExecutor execute_element, void *execution_context,
    SourceLocation location, std::string_view producer_name) {
  const auto release_completed = [&resources, &element_storage](
                                     std::size_t completed_count) {
    for (std::size_t offset = completed_count; offset > 0U; --offset) {
      destroy_proven_value(element_storage[offset - 1U]);
    }
  };
  if (execute_element == nullptr) {
    Error error = make_error(ErrorKind::domain_error, location);
    error.static_message = "tuple element executor is null";
    error.primitive =
        make_primitive_error_context(producer_name, std::nullopt);
    return TupleConstructionResult{false, make_int_value(0),
                                   ValueInvariant::none, std::move(error)};
  }

  for (const Value &scratch : element_storage) {
    if (!scratch.claimed) {
      continue;
    }
    const ValueValidationResult validation = validate_value(scratch);
    if (!validation.ok) {
      Error error;
      if (validation.resource_error != HostResourceErrorReason::none) {
        error = make_resource_failure(
            resources,
            validation.resource_error == HostResourceErrorReason::size_overflow
                ? ResourceErrorReason::size_overflow
                : ResourceErrorReason::allocation_unavailable,
            location, producer_name, element_storage.size(), std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt);
      } else {
        error = make_error(ErrorKind::invalid_value, location);
        error.static_message =
            "tuple construction scratch contains an invalid owner";
      }
      return TupleConstructionResult{
          false, make_int_value(0), validation.invariant,
          std::move(error)};
    }
  }
  for (Value &scratch : element_storage) {
    destroy_proven_value(scratch);
  }

  std::size_t completed_count = 0U;
  for (std::size_t element_index = 0U;
       element_index < element_storage.size(); ++element_index) {
    TupleElementExecutionResult produced =
        execute_element(execution_context, resources, element_index);
    if (!produced.ok) {
      if (produced.value.claimed) {
        const ValueValidationResult validation =
            validate_value(produced.value);
        if (validation.ok) {
          destroy_proven_value(produced.value);
        } else {
          element_storage[element_index] =
              move_value(produced.value);
        }
      }
      release_completed(completed_count);
      return TupleConstructionResult{false, make_int_value(0),
                                     ValueInvariant::none,
                                     std::move(produced.error)};
    }
    const ValueValidationResult produced_validation =
        validate_value(produced.value);
    if (!produced_validation.ok) {
      element_storage[element_index] = move_value(produced.value);
      release_completed(completed_count);
      Error error;
      if (produced_validation.resource_error !=
          HostResourceErrorReason::none) {
        error = make_resource_failure(
            resources,
            produced_validation.resource_error ==
                    HostResourceErrorReason::size_overflow
                ? ResourceErrorReason::size_overflow
                : ResourceErrorReason::allocation_unavailable,
            location, producer_name, element_storage.size(), std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt);
      } else {
        error = make_error(ErrorKind::invalid_value, location);
        error.static_message =
            "tuple element executor returned an invalid owner";
      }
      return TupleConstructionResult{
          false, make_int_value(0), produced_validation.invariant,
          std::move(error)};
    }
    element_storage[element_index] = move_value(produced.value);
    ++completed_count;
  }

  TupleConstructionResult tuple = make_tuple_value(
      resources, element_storage, location, producer_name);
  if (!tuple.ok) {
    release_completed(completed_count);
  }
  return tuple;
}

ValueReleaseResult release_vector_reservation(EvaluationResources &resources,
                                              Value &value) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return release_vector_reservation(resources, value, allocation_failure);
}

ValueReleaseResult release_vector_reservation(
    EvaluationResources &resources, Value &value,
    HostAllocationFailureInjection &allocation_failure) {
  if (value.container != ContainerKind::vector) {
    return ValueReleaseResult{false, ValueInvariant::none};
  }
  const ValueValidationResult validation =
      validate_value(value, allocation_failure);
  if (!validation.ok) {
    ValueReleaseResult result{false, validation.invariant};
    result.resource_error = validation.resource_error;
    return result;
  }
  if (value.vector.accounting_active &&
      !same_owner(value.vector.accounting_owner, resources.owner)) {
    return ValueReleaseResult{false, ValueInvariant::none,
                              ValueReleaseError::resource_context_mismatch};
  }
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  if (resource_state == nullptr) {
    return ValueReleaseResult{false, ValueInvariant::none,
                              ValueReleaseError::resource_context_mismatch};
  }
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }
  const ResourceLifetimeObserver observer =
      value.vector.lifetime_observer;
  const std::optional<std::size_t> ordinal =
      value.vector.allocation_ordinal;
  const void *storage = vector_storage(value.vector);
  const std::size_t canonical_bytes =
      value.vector.canonical_bytes;
  bool logical_release = false;
  std::size_t element_count = 0;
  switch (value.vector.element_type) {
  case ScalarType::boolean:
    element_count = value.vector.boolean_count;
    break;
  case ScalarType::integer:
    element_count = value.vector.integer_count;
    break;
  case ScalarType::double_precision:
    element_count = value.vector.double_count;
    break;
  }
  const std::optional<std::size_t> width =
      element_width(value.vector.element_type);
  if (width.has_value() &&
      element_count <= std::numeric_limits<std::size_t>::max() / *width) {
    const std::size_t bytes = element_count * *width;
    if (value.vector.accounting_active &&
        same_owner(value.vector.accounting_owner, resources.owner) &&
        bytes == value.vector.canonical_bytes &&
        bytes <= resources.live_evaluation_bytes) {
      resources.live_evaluation_bytes -= bytes;
      logical_release = true;
    }
  }
  const EvaluationResourceOwner accounting_owner =
      value.vector.accounting_owner;
  value.vector.accounting_active = false;
  value.vector.accounting_owner = EvaluationResourceOwner{};
  detail::release_resource_state_owner(accounting_owner);
  const bool physical_release = storage != nullptr;
  if (storage != nullptr) {
    reset_vector_storage(value.vector);
  }
  value.vector.allocation_ordinal = std::nullopt;
  value.vector.lifetime_observer = ResourceLifetimeObserver{};
  value = make_int_value(0);
  if (transaction.owns_lock()) {
    transaction.unlock();
  }
  if (logical_release) {
    observe_lifetime(observer, ResourceLifetimeEventKind::logical_release,
                     ResourceStorageKind::vector_payload, ordinal, storage,
                     canonical_bytes);
  }
  if (physical_release) {
    observe_lifetime(observer, ResourceLifetimeEventKind::physical_release,
                     ResourceStorageKind::vector_payload, ordinal, storage,
                     canonical_bytes);
  }
  return ValueReleaseResult{true, ValueInvariant::none};
}

void release_workspace(EvaluationResources &resources,
                       WorkspaceReservation &reservation) {
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  if (resource_state == nullptr) {
    reservation.storage.reset();
    reservation.bytes = 0U;
    return;
  }
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }
  if (reservation.bytes <= resources.live_evaluation_bytes) {
    resources.live_evaluation_bytes -= reservation.bytes;
  }
  reservation.storage.reset();
  reservation.bytes = 0;
}

ValueReleaseResult release_value_reservations(EvaluationResources &resources,
                                              Value &value) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return release_value_reservations(resources, value, allocation_failure);
}

ValueReleaseResult release_value_reservations(
    EvaluationResources &resources, Value &value,
    HostAllocationFailureInjection &allocation_failure) {
  const ValueValidationResult validation =
      validate_value(value, allocation_failure);
  if (!validation.ok) {
    ValueReleaseResult result{false, validation.invariant};
    result.resource_error = validation.resource_error;
    return result;
  }
  if (value.container == ContainerKind::scalar) {
    (void)destroy_value(value);
    return ValueReleaseResult{true, ValueInvariant::none};
  }
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  if (resource_state == nullptr) {
    return ValueReleaseResult{false, ValueInvariant::none,
                              ValueReleaseError::resource_context_mismatch};
  }
  const EvaluationResourceOwner expected_owner = resources.owner;
  if (value.container == ContainerKind::vector &&
      value.vector.accounting_active &&
      !same_owner(value.vector.accounting_owner, expected_owner)) {
    return ValueReleaseResult{false, ValueInvariant::none,
                              ValueReleaseError::resource_context_mismatch};
  }
  if (value.container == ContainerKind::tuple) {
    for (const VectorValue &payload :
         host_array_span(value.tuple.vector_payloads)) {
      if (payload.accounting_active &&
          !same_owner(payload.accounting_owner, expected_owner)) {
        return ValueReleaseResult{false, ValueInvariant::none,
                                  ValueReleaseError::resource_context_mismatch};
      }
    }
    for (const TupleTableReservation &reservation :
         host_array_span(value.tuple.reservations)) {
      if (reservation.accounting_active &&
          reservation.canonical_bytes != 0U &&
          !same_owner(reservation.accounting_owner, expected_owner)) {
        return ValueReleaseResult{false, ValueInvariant::none,
                                  ValueReleaseError::resource_context_mismatch};
      }
    }
    if (value.tuple.root_reservation.accounting_active &&
        value.tuple.root_reservation.canonical_bytes != 0U &&
        !same_owner(value.tuple.root_reservation.accounting_owner,
                    expected_owner)) {
      return ValueReleaseResult{false, ValueInvariant::none,
                                ValueReleaseError::resource_context_mismatch};
    }
  }
  if (value.container == ContainerKind::vector) {
    return release_vector_reservation(resources, value);
  }
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }

  const auto set_parent = [&value](std::size_t child_index,
                                   std::size_t parent_index) {
    ValueNode &child = value.tuple.nodes.storage.get()[child_index];
    if (child.container == ContainerKind::tuple) {
      child.vector_payload_index = parent_index;
    } else {
      child.first_child = parent_index;
    }
  };
  for (std::size_t node_index = 0U; node_index < value.tuple.nodes.size;
       ++node_index) {
    const ValueNode &node = value.tuple.nodes.storage.get()[node_index];
    if (node.container != ContainerKind::tuple) {
      continue;
    }
    for (std::size_t offset = 0U; offset < node.child_count; ++offset) {
      set_parent(value.tuple.child_indexes.storage.get()[
                     node.first_child + offset],
                 node_index);
    }
  }
  for (std::size_t offset = 0U; offset < value.tuple.child_count; ++offset) {
    set_parent(value.tuple.child_indexes.storage.get()[
                   value.tuple.first_child + offset],
               value.tuple.root_index);
  }

  std::size_t node_index = value.tuple.root_index;
  while (true) {
    const bool root = node_index == value.tuple.root_index;
    ValueNode *node =
        root ? nullptr : &value.tuple.nodes.storage.get()[node_index];
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
            value.tuple.child_indexes.storage.get()[
                first_child + remaining_children];
        continue;
      }

      TupleTableReservation &reservation =
          root ? value.tuple.root_reservation
               : value.tuple.reservations.storage.get()[
                     node->tuple_reservation_index];
      if (reservation.accounting_active &&
          reservation.canonical_bytes <= resources.live_evaluation_bytes) {
        resources.live_evaluation_bytes -= reservation.canonical_bytes;
        observe_lifetime_unlocked(
            transaction, reservation.lifetime_observer,
            ResourceLifetimeEventKind::logical_release,
            ResourceStorageKind::tuple_table,
            reservation.allocation_ordinal,
            reservation.storage.get(), reservation.canonical_bytes);
      }
      const EvaluationResourceOwner accounting_owner =
          reservation.accounting_owner;
      reservation.accounting_active = false;
      reservation.accounting_owner = EvaluationResourceOwner{};
      detail::release_resource_state_owner(accounting_owner);
      const void *storage = reservation.storage.get();
      if (storage != nullptr) {
        observe_lifetime_unlocked(
            transaction, reservation.lifetime_observer,
            ResourceLifetimeEventKind::physical_release,
            ResourceStorageKind::tuple_table,
            reservation.allocation_ordinal, storage,
            reservation.canonical_bytes);
        reservation.storage.reset();
      }
      reservation.allocation_ordinal = std::nullopt;
      reservation.lifetime_observer = ResourceLifetimeObserver{};
    } else if (container == ContainerKind::vector) {
      VectorValue &vector =
          value.tuple.vector_payloads.storage.get()[
              node->vector_payload_index];
      std::size_t element_count = 0U;
      switch (vector.element_type) {
      case ScalarType::boolean:
        element_count = vector.boolean_count;
        break;
      case ScalarType::integer:
        element_count = vector.integer_count;
        break;
      case ScalarType::double_precision:
        element_count = vector.double_count;
        break;
      }
      const std::optional<std::size_t> width = element_width(vector.element_type);
      if (width.has_value() &&
          element_count <= std::numeric_limits<std::size_t>::max() / *width) {
        const std::size_t bytes = element_count * *width;
        if (vector.accounting_active && bytes == vector.canonical_bytes &&
            bytes <= resources.live_evaluation_bytes) {
          resources.live_evaluation_bytes -= bytes;
          observe_lifetime_unlocked(
              transaction, vector.lifetime_observer,
              ResourceLifetimeEventKind::logical_release,
              ResourceStorageKind::vector_payload,
              vector.allocation_ordinal, vector_storage(vector), bytes);
        }
        const EvaluationResourceOwner accounting_owner =
            vector.accounting_owner;
        vector.accounting_active = false;
        vector.accounting_owner = EvaluationResourceOwner{};
        detail::release_resource_state_owner(accounting_owner);
        const void *storage = vector_storage(vector);
        if (storage != nullptr) {
          observe_lifetime_unlocked(
              transaction, vector.lifetime_observer,
              ResourceLifetimeEventKind::physical_release,
              ResourceStorageKind::vector_payload,
              vector.allocation_ordinal, storage,
              vector.canonical_bytes);
          reset_vector_storage(vector);
        }
        vector.allocation_ordinal = std::nullopt;
        vector.lifetime_observer = ResourceLifetimeObserver{};
      }
    }

    if (root) {
      break;
    }
    node_index = container == ContainerKind::tuple ? node->vector_payload_index
                                                   : node->first_child;
  }
  Value empty_owner = make_int_value(0);
  (void)destroy_value(empty_owner);
  value = std::move(empty_owner);
  return ValueReleaseResult{true, ValueInvariant::none};
}

ValueReleaseResult detach_value_reservations(EvaluationResources &resources,
                                             Value &value) {
  const ValueValidationResult validation = validate_value(value);
  if (!validation.ok) {
    ValueReleaseResult result{false, validation.invariant};
    result.resource_error = validation.resource_error;
    return result;
  }
  if (value.container == ContainerKind::scalar) {
    return ValueReleaseResult{true, ValueInvariant::none};
  }
  EvaluationResourceState *resource_state =
      validated_resource_state(resources);
  if (resource_state == nullptr) {
    return ValueReleaseResult{false, ValueInvariant::none,
                              ValueReleaseError::resource_context_mismatch};
  }
  const EvaluationResourceOwner expected_owner = resources.owner;
  if (value.container == ContainerKind::vector) {
    if (value.vector.accounting_active &&
        !same_owner(value.vector.accounting_owner, expected_owner)) {
      return ValueReleaseResult{false, ValueInvariant::none,
                                ValueReleaseError::resource_context_mismatch};
    }
    std::unique_lock<std::mutex> transaction;
    if (resource_state != nullptr) {
      transaction =
          std::unique_lock<std::mutex>(resource_state->transaction_mutex);
    }
    if (value.vector.accounting_active &&
        value.vector.canonical_bytes <= resources.live_evaluation_bytes) {
      resources.live_evaluation_bytes -= value.vector.canonical_bytes;
      observe_lifetime_unlocked(
          transaction, value.vector.lifetime_observer,
          ResourceLifetimeEventKind::logical_release,
          ResourceStorageKind::vector_payload,
          value.vector.allocation_ordinal,
          vector_storage(value.vector),
          value.vector.canonical_bytes);
    }
    const EvaluationResourceOwner accounting_owner =
        value.vector.accounting_owner;
    value.vector.accounting_active = false;
    value.vector.accounting_owner = EvaluationResourceOwner{};
    detail::release_resource_state_owner(accounting_owner);
    return ValueReleaseResult{true, ValueInvariant::none};
  }
  for (const VectorValue &payload :
       host_array_span(value.tuple.vector_payloads)) {
    if (payload.accounting_active &&
        !same_owner(payload.accounting_owner, expected_owner)) {
      return ValueReleaseResult{false, ValueInvariant::none,
                                ValueReleaseError::resource_context_mismatch};
    }
  }
  for (const TupleTableReservation &reservation :
       host_array_span(value.tuple.reservations)) {
    if (reservation.accounting_active &&
        reservation.canonical_bytes != 0U &&
        !same_owner(reservation.accounting_owner, expected_owner)) {
      return ValueReleaseResult{false, ValueInvariant::none,
                                ValueReleaseError::resource_context_mismatch};
    }
  }
  if (value.tuple.root_reservation.accounting_active &&
      value.tuple.root_reservation.canonical_bytes != 0U &&
      !same_owner(value.tuple.root_reservation.accounting_owner,
                  expected_owner)) {
    return ValueReleaseResult{false, ValueInvariant::none,
                              ValueReleaseError::resource_context_mismatch};
  }
  std::unique_lock<std::mutex> transaction;
  if (resource_state != nullptr) {
    transaction =
        std::unique_lock<std::mutex>(resource_state->transaction_mutex);
  }

  const auto set_parent = [&value](std::size_t child_index,
                                   std::size_t parent_index) {
    ValueNode &child = value.tuple.nodes.storage.get()[child_index];
    if (child.container == ContainerKind::tuple) {
      child.vector_payload_index = parent_index;
    } else {
      child.first_child = parent_index;
    }
  };
  for (std::size_t node_index = 0U; node_index < value.tuple.nodes.size;
       ++node_index) {
    const ValueNode &node = value.tuple.nodes.storage.get()[node_index];
    if (node.container != ContainerKind::tuple) {
      continue;
    }
    for (std::size_t offset = 0U; offset < node.child_count; ++offset) {
      set_parent(value.tuple.child_indexes.storage.get()[
                     node.first_child + offset],
                 node_index);
    }
  }
  for (std::size_t offset = 0U; offset < value.tuple.child_count; ++offset) {
    set_parent(value.tuple.child_indexes.storage.get()[
                   value.tuple.first_child + offset],
               value.tuple.root_index);
  }

  std::size_t node_index = value.tuple.root_index;
  while (true) {
    const bool root = node_index == value.tuple.root_index;
    ValueNode *node =
        root ? nullptr : &value.tuple.nodes.storage.get()[node_index];
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
            value.tuple.child_indexes.storage.get()[
                first_child + remaining_children];
        continue;
      }
      TupleTableReservation &reservation =
          root ? value.tuple.root_reservation
               : value.tuple.reservations.storage.get()[
                     node->tuple_reservation_index];
      if (reservation.accounting_active &&
          reservation.canonical_bytes <= resources.live_evaluation_bytes) {
        resources.live_evaluation_bytes -= reservation.canonical_bytes;
        observe_lifetime_unlocked(
            transaction, reservation.lifetime_observer,
            ResourceLifetimeEventKind::logical_release,
            ResourceStorageKind::tuple_table,
            reservation.allocation_ordinal,
            reservation.storage.get(), reservation.canonical_bytes);
      }
      const EvaluationResourceOwner accounting_owner =
          reservation.accounting_owner;
      reservation.accounting_active = false;
      reservation.accounting_owner = EvaluationResourceOwner{};
      detail::release_resource_state_owner(accounting_owner);
    } else if (container == ContainerKind::vector) {
      VectorValue &vector =
          value.tuple.vector_payloads.storage.get()[
              node->vector_payload_index];
      if (vector.accounting_active &&
          vector.canonical_bytes <= resources.live_evaluation_bytes) {
        resources.live_evaluation_bytes -= vector.canonical_bytes;
        observe_lifetime_unlocked(
            transaction, vector.lifetime_observer,
            ResourceLifetimeEventKind::logical_release,
            ResourceStorageKind::vector_payload,
            vector.allocation_ordinal, vector_storage(vector),
            vector.canonical_bytes);
      }
      const EvaluationResourceOwner accounting_owner =
          vector.accounting_owner;
      vector.accounting_active = false;
      vector.accounting_owner = EvaluationResourceOwner{};
      detail::release_resource_state_owner(accounting_owner);
    }

    if (root) {
      break;
    }
    node_index = container == ContainerKind::tuple ? node->vector_payload_index
                                                   : node->first_child;
  }

  value.tuple.child_count = value.tuple.root_reservation.element_count;
  for (ValueNode &node : host_array_span(value.tuple.nodes)) {
    if (node.container == ContainerKind::tuple) {
      node.child_count =
          value.tuple.reservations.storage
              .get()[node.tuple_reservation_index]
              .element_count;
      node.vector_payload_index = 0U;
    } else {
      node.first_child = 0U;
    }
  }
  return ValueReleaseResult{true, ValueInvariant::none};
}

namespace {

constexpr SourceLocation test_location{7, 2, 3};
constexpr AllocationFailureInjection no_allocation_failure{std::nullopt};

ResourceLimits limits(std::optional<std::size_t> vector_bytes,
                      std::optional<std::size_t> live_bytes,
                      std::optional<std::size_t> work_units) {
  return ResourceLimits{vector_bytes, live_bytes, work_units};
}

[[maybe_unused]] void
check_profile_refusal(const Error &error, ResourceLimitKind limit_kind,
                      std::size_t configured_limit, std::size_t usage_before,
                      std::size_t refused_charge) {
  (void)error;
  (void)limit_kind;
  (void)configured_limit;
  (void)usage_before;
  (void)refused_charge;
  CHECK(error.kind == ErrorKind::resource_error);
  CHECK(error.location.offset == test_location.offset);
  CHECK(error.location.line == test_location.line);
  CHECK(error.location.column == test_location.column);
  REQUIRE(error.resource.has_value());
  CHECK(error.resource->reason == ResourceErrorReason::profile_limit);
  CHECK(error.resource->profile == "bounded-v1");
  REQUIRE(error.resource->limit_kind.has_value());
  CHECK(*error.resource->limit_kind == limit_kind);
  REQUIRE(error.resource->configured_limit.has_value());
  CHECK(*error.resource->configured_limit == configured_limit);
  REQUIRE(error.resource->usage_before.has_value());
  CHECK(*error.resource->usage_before == usage_before);
  REQUIRE(error.resource->refused_charge.has_value());
  CHECK(*error.resource->refused_charge == refused_charge);
}

void check_invalid_profile_error(const Error &error,
                                 std::string_view expected_message,
                                 std::string_view producer_name) {
  (void)error;
  (void)expected_message;
  (void)producer_name;
  CHECK(error.kind == ErrorKind::invalid_execution_profile);
  CHECK(error.location.offset == test_location.offset);
  CHECK(error.location.line == test_location.line);
  CHECK(error.location.column == test_location.column);
  CHECK(error.static_message == expected_message);
  REQUIRE(error.primitive.has_value());
  CHECK(error.primitive->name == std::string(producer_name));
  CHECK_FALSE(error.resource.has_value());
}

} // namespace

TEST_CASE("malformed profile configurations refuse every admission entry") {
  const AllocationFailureInjection fail_first{std::size_t{0}};

  EvaluationResources bounded_without_limits =
      make_bounded_resources(limits(std::nullopt, std::nullopt, std::nullopt),
                             fail_first);
  EvaluationResources trusted_with_vector_limit =
      make_trusted_local_resources(fail_first);
  trusted_with_vector_limit.limits.max_vector_bytes = 0;
  EvaluationResources trusted_with_live_limit =
      make_trusted_local_resources(fail_first);
  trusted_with_live_limit.limits.max_live_evaluation_bytes = 0;
  EvaluationResources trusted_with_work_limit =
      make_trusted_local_resources(fail_first);
  trusted_with_work_limit.limits.max_work_units = 0;
  EvaluationResources unknown_profile =
      make_trusted_local_resources(fail_first);
  unknown_profile.profile = static_cast<ExecutionProfile>(99);

  const std::array<EvaluationResources, 5> malformed{{
      bounded_without_limits,
      trusted_with_vector_limit,
      trusted_with_live_limit,
      trusted_with_work_limit,
      unknown_profile,
  }};
  constexpr std::array<std::string_view, 5> expected_messages{{
      "bounded-v1 requires at least one configured resource limit",
      "trusted-local-v1 requires every resource limit to be omitted",
      "trusted-local-v1 requires every resource limit to be omitted",
      "trusted-local-v1 requires every resource limit to be omitted",
      "execution profile tag is unknown",
  }};

  for (std::size_t index = 0; index < malformed.size(); ++index) {
    CAPTURE(index);

    EvaluationResources vector_resources = malformed[index];
    const VectorAllocationResult vector = allocate_vector(
        vector_resources, ScalarType::boolean, 1, 0, test_location,
        "vector-literal");
    CHECK_FALSE(vector.ok);
    check_invalid_profile_error(vector.error, expected_messages[index],
                                "vector-literal");
    CHECK(vector.value.container == ContainerKind::scalar);
    CHECK(validate_value(vector.value).ok);
    CHECK(vector_resources.live_evaluation_bytes == 0);
    CHECK(vector_resources.work_units == 0);
    CHECK(vector_resources.reservation_ordinal == 0);

    EvaluationResources sizing_resources = malformed[index];
    const VectorAllocationResult sizing = allocate_vector(
        sizing_resources, ScalarType::integer,
        std::numeric_limits<std::uint64_t>::max(), 0, test_location,
        "sizing-probe");
    CHECK_FALSE(sizing.ok);
    check_invalid_profile_error(sizing.error, expected_messages[index],
                                "sizing-probe");
    CHECK(sizing.value.container == ContainerKind::scalar);
    CHECK(validate_value(sizing.value).ok);
    CHECK(sizing_resources.live_evaluation_bytes == 0);
    CHECK(sizing_resources.work_units == 0);
    CHECK(sizing_resources.reservation_ordinal == 0);

    EvaluationResources workspace_resources = malformed[index];
    const WorkspaceReservationResult workspace = reserve_workspace(
        workspace_resources, 1, 0, test_location, "sort-workspace");
    CHECK_FALSE(workspace.ok);
    check_invalid_profile_error(workspace.error, expected_messages[index],
                                "sort-workspace");
    CHECK(workspace.reservation.storage.get() == nullptr);
    CHECK(workspace.reservation.bytes == 0);
    CHECK(workspace_resources.live_evaluation_bytes == 0);
    CHECK(workspace_resources.work_units == 0);
    CHECK(workspace_resources.reservation_ordinal == 0);

    EvaluationResources work_resources = malformed[index];
    const WorkChargeResult work =
        charge_work(work_resources, 0, test_location, "inc");
    CHECK_FALSE(work.ok);
    check_invalid_profile_error(work.error, expected_messages[index], "inc");
    CHECK(work_resources.live_evaluation_bytes == 0);
    CHECK(work_resources.work_units == 0);
    CHECK(work_resources.reservation_ordinal == 0);
  }
}

TEST_CASE("neighboring valid profile configurations remain operational") {
  EvaluationResources trusted =
      make_trusted_local_resources(no_allocation_failure);
  CHECK_FALSE(trusted.limits.max_vector_bytes.has_value());
  CHECK_FALSE(trusted.limits.max_live_evaluation_bytes.has_value());
  CHECK_FALSE(trusted.limits.max_work_units.has_value());
  VectorAllocationResult trusted_vector = allocate_vector(
      trusted, ScalarType::boolean, 1, 0, test_location, "vector-literal");
  REQUIRE(trusted_vector.ok);
  WorkspaceReservationResult trusted_workspace =
      reserve_workspace(trusted, 1, 0, test_location, "sort-workspace");
  REQUIRE(trusted_workspace.ok);
  REQUIRE(charge_work(trusted, 1, test_location, "inc").ok);
  release_vector_reservation(trusted, trusted_vector.value);
  release_workspace(trusted, trusted_workspace.reservation);
  CHECK(trusted.live_evaluation_bytes == 0);
  CHECK(trusted.work_units == 1);

  EvaluationResources bounded_vector = make_bounded_resources(
      limits(1, std::nullopt, std::nullopt), no_allocation_failure);
  VectorAllocationResult bounded_vector_result = allocate_vector(
      bounded_vector, ScalarType::boolean, 1, 0, test_location,
      "vector-literal");
  REQUIRE(bounded_vector_result.ok);
  release_vector_reservation(bounded_vector, bounded_vector_result.value);
  CHECK(bounded_vector.live_evaluation_bytes == 0);

  EvaluationResources bounded_live = make_bounded_resources(
      limits(std::nullopt, 1, std::nullopt), no_allocation_failure);
  WorkspaceReservationResult bounded_workspace =
      reserve_workspace(bounded_live, 1, 0, test_location, "sort-workspace");
  REQUIRE(bounded_workspace.ok);
  release_workspace(bounded_live, bounded_workspace.reservation);
  CHECK(bounded_live.live_evaluation_bytes == 0);

  EvaluationResources bounded_work = make_bounded_resources(
      limits(std::nullopt, std::nullopt, 1), no_allocation_failure);
  REQUIRE(charge_work(bounded_work, 1, test_location, "inc").ok);
  CHECK(bounded_work.work_units == 1);
}

TEST_CASE("vector boundary enforces zero exact and one-past profile limits") {
  EvaluationResources zero = make_bounded_resources(
      limits(0, std::nullopt, std::nullopt), no_allocation_failure);
  VectorAllocationResult empty = allocate_vector(
      zero, ScalarType::integer, 0, 0, test_location, "vector-literal");
  REQUIRE(empty.ok);
  std::size_t empty_length = 99;
  REQUIRE(value_length(empty.value, empty_length).ok);
  CHECK(empty_length == 0);
  CHECK(zero.live_evaluation_bytes == 0);
  CHECK(zero.work_units == 0);
  CHECK(zero.reservation_ordinal == 0);

  VectorAllocationResult refused = allocate_vector(
      zero, ScalarType::integer, 1, 0, test_location, "vector-literal");
  CHECK_FALSE(refused.ok);
  CHECK(refused.value.container == ContainerKind::scalar);
  check_profile_refusal(refused.error, ResourceLimitKind::max_vector_bytes, 0,
                        0, 8);
  CHECK(zero.live_evaluation_bytes == 0);
  CHECK(zero.work_units == 0);
  CHECK(zero.reservation_ordinal == 0);

  EvaluationResources exact = make_bounded_resources(
      limits(8, 8, 1), no_allocation_failure);
  VectorAllocationResult admitted = allocate_vector(
      exact, ScalarType::integer, 1, 1, test_location, "lifted-inc");
  REQUIRE(admitted.ok);
  CHECK(exact.live_evaluation_bytes == 8);
  CHECK(exact.work_units == 1);
  CHECK(exact.reservation_ordinal == 1);

  VectorAllocationResult one_past = allocate_vector(
      exact, ScalarType::boolean, 1, 0, test_location, "vector-literal");
  CHECK_FALSE(one_past.ok);
  check_profile_refusal(one_past.error,
                        ResourceLimitKind::max_live_evaluation_bytes, 8, 8, 1);
  CHECK(exact.live_evaluation_bytes == 8);
  CHECK(exact.work_units == 1);
  CHECK(exact.reservation_ordinal == 1);
}

TEST_CASE("each profile limit honors zero one exact and one-past semantics") {
  EvaluationResources vector_zero = make_bounded_resources(
      limits(0, std::nullopt, std::nullopt), no_allocation_failure);
  REQUIRE(allocate_vector(vector_zero, ScalarType::boolean, 0, 0,
                          test_location, "vector-literal")
              .ok);
  const VectorAllocationResult vector_zero_refused = allocate_vector(
      vector_zero, ScalarType::boolean, 1, 0, test_location, "vector-literal");
  CHECK_FALSE(vector_zero_refused.ok);
  check_profile_refusal(vector_zero_refused.error,
                        ResourceLimitKind::max_vector_bytes, 0, 0, 1);

  EvaluationResources vector_one = make_bounded_resources(
      limits(1, std::nullopt, std::nullopt), no_allocation_failure);
  REQUIRE(allocate_vector(vector_one, ScalarType::boolean, 1, 0,
                          test_location, "vector-literal")
              .ok);
  const VectorAllocationResult vector_one_past = allocate_vector(
      vector_one, ScalarType::boolean, 2, 0, test_location, "vector-literal");
  CHECK_FALSE(vector_one_past.ok);
  check_profile_refusal(vector_one_past.error,
                        ResourceLimitKind::max_vector_bytes, 1, 0, 2);

  EvaluationResources live_zero = make_bounded_resources(
      limits(std::nullopt, 0, std::nullopt), no_allocation_failure);
  REQUIRE(allocate_vector(live_zero, ScalarType::boolean, 0, 0,
                          test_location, "vector-literal")
              .ok);
  const VectorAllocationResult live_zero_refused = allocate_vector(
      live_zero, ScalarType::boolean, 1, 0, test_location, "vector-literal");
  CHECK_FALSE(live_zero_refused.ok);
  check_profile_refusal(live_zero_refused.error,
                        ResourceLimitKind::max_live_evaluation_bytes, 0, 0, 1);

  EvaluationResources live_one = make_bounded_resources(
      limits(std::nullopt, 1, std::nullopt), no_allocation_failure);
  REQUIRE(allocate_vector(live_one, ScalarType::boolean, 1, 0,
                          test_location, "vector-literal")
              .ok);
  const VectorAllocationResult live_one_past = allocate_vector(
      live_one, ScalarType::boolean, 1, 0, test_location, "vector-literal");
  CHECK_FALSE(live_one_past.ok);
  check_profile_refusal(live_one_past.error,
                        ResourceLimitKind::max_live_evaluation_bytes, 1, 1, 1);

  EvaluationResources work_zero = make_bounded_resources(
      limits(std::nullopt, std::nullopt, 0), no_allocation_failure);
  REQUIRE(charge_work(work_zero, 0, test_location, "inc").ok);
  const WorkChargeResult work_zero_refused =
      charge_work(work_zero, 1, test_location, "inc");
  CHECK_FALSE(work_zero_refused.ok);
  check_profile_refusal(work_zero_refused.error,
                        ResourceLimitKind::max_work_units, 0, 0, 1);

  EvaluationResources work_one = make_bounded_resources(
      limits(std::nullopt, std::nullopt, 1), no_allocation_failure);
  REQUIRE(charge_work(work_one, 1, test_location, "inc").ok);
  const WorkChargeResult work_one_past =
      charge_work(work_one, 1, test_location, "inc");
  CHECK_FALSE(work_one_past.ok);
  check_profile_refusal(work_one_past.error,
                        ResourceLimitKind::max_work_units, 1, 1, 1);
}

TEST_CASE("resource sizing rejects multiplication and cumulative overflow") {
  EvaluationResources resources =
      make_trusted_local_resources(no_allocation_failure);
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();

  VectorAllocationResult multiplication = allocate_vector(
      resources, ScalarType::integer, maximum, 0, test_location, "iota");
  CHECK_FALSE(multiplication.ok);
  REQUIRE(multiplication.error.resource.has_value());
  CHECK(multiplication.error.resource->reason ==
        ResourceErrorReason::size_overflow);
  REQUIRE(multiplication.error.resource->requested_elements.has_value());
  CHECK(*multiplication.error.resource->requested_elements == maximum);
  CHECK_FALSE(multiplication.error.resource->requested_bytes.has_value());
  CHECK(resources.reservation_ordinal == 0);

  resources.live_evaluation_bytes = maximum - 3;
  VectorAllocationResult cumulative = allocate_vector(
      resources, ScalarType::boolean, 4, 0, test_location, "vector-literal");
  CHECK_FALSE(cumulative.ok);
  REQUIRE(cumulative.error.resource.has_value());
  CHECK(cumulative.error.resource->reason ==
        ResourceErrorReason::size_overflow);
  REQUIRE(cumulative.error.resource->requested_bytes.has_value());
  CHECK(*cumulative.error.resource->requested_bytes == 4);
  CHECK(resources.live_evaluation_bytes == maximum - 3);
  CHECK(resources.reservation_ordinal == 0);

  resources.live_evaluation_bytes = 0;
  resources.work_units = maximum;
  const WorkChargeResult work =
      charge_work(resources, 1, test_location, "inc");
  CHECK_FALSE(work.ok);
  REQUIRE(work.error.resource.has_value());
  CHECK(work.error.resource->reason == ResourceErrorReason::size_overflow);
  CHECK(resources.work_units == maximum);

  EvaluationResources maximum_count = make_trusted_local_resources(
      AllocationFailureInjection{std::size_t{0}});
  VectorAllocationResult representable = allocate_vector(
      maximum_count, ScalarType::boolean, maximum, 0, test_location,
      "vector-literal");
  CHECK_FALSE(representable.ok);
  REQUIRE(representable.error.resource.has_value());
  CHECK(representable.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  REQUIRE(representable.error.resource->requested_elements.has_value());
  CHECK(*representable.error.resource->requested_elements == maximum);
  REQUIRE(representable.error.resource->requested_bytes.has_value());
  CHECK(*representable.error.resource->requested_bytes == maximum);
  CHECK(maximum_count.live_evaluation_bytes == 0);
  CHECK(maximum_count.work_units == 0);
  CHECK(maximum_count.reservation_ordinal == 1);
}

TEST_CASE("allocation failure injection is ordinal and transactional") {
  EvaluationResources resources = make_trusted_local_resources(
      AllocationFailureInjection{std::size_t{1}});

  VectorAllocationResult first = allocate_vector(
      resources, ScalarType::boolean, 1, 1, test_location, "vector-literal");
  REQUIRE(first.ok);
  CHECK(resources.live_evaluation_bytes == 1);
  CHECK(resources.work_units == 1);
  CHECK(resources.reservation_ordinal == 1);

  VectorAllocationResult second = allocate_vector(
      resources, ScalarType::double_precision, 1, 4, test_location,
      "lifted-add");
  CHECK_FALSE(second.ok);
  REQUIRE(second.error.resource.has_value());
  CHECK(second.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  REQUIRE(second.error.resource->requested_elements.has_value());
  CHECK(*second.error.resource->requested_elements == 1);
  REQUIRE(second.error.resource->requested_bytes.has_value());
  CHECK(*second.error.resource->requested_bytes == 8);
  CHECK(second.value.container == ContainerKind::scalar);
  CHECK(resources.live_evaluation_bytes == 1);
  CHECK(resources.work_units == 1);
  CHECK(resources.reservation_ordinal == 2);
}

TEST_CASE("vector copies and generic workspace share the allocation seam") {
  const std::array<std::uint8_t, 1> boolean_values{{1}};
  EvaluationResources vector_resources = make_trusted_local_resources(
      AllocationFailureInjection{std::size_t{0}});
  const VectorAllocationResult vector = copy_bool_vector(
      vector_resources, boolean_values, test_location, "vector-literal");
  CHECK_FALSE(vector.ok);
  CHECK(vector.value.container == ContainerKind::scalar);
  REQUIRE(vector.error.resource.has_value());
  CHECK(vector.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  REQUIRE(vector.error.primitive.has_value());
  CHECK(vector.error.primitive->name == "vector-literal");
  CHECK(vector_resources.live_evaluation_bytes == 0);
  CHECK(vector_resources.work_units == 0);
  CHECK(vector_resources.reservation_ordinal == 1);

  EvaluationResources workspace_resources = make_trusted_local_resources(
      AllocationFailureInjection{std::size_t{0}});
  const WorkspaceReservationResult workspace = reserve_workspace(
      workspace_resources, 1, 0, test_location, "sort-workspace");
  CHECK_FALSE(workspace.ok);
  CHECK(workspace.reservation.storage.get() == nullptr);
  CHECK(workspace.reservation.bytes == 0);
  REQUIRE(workspace.error.resource.has_value());
  CHECK(workspace.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  REQUIRE(workspace.error.primitive.has_value());
  CHECK(workspace.error.primitive->name == "sort-workspace");
  CHECK(workspace_resources.live_evaluation_bytes == 0);
  CHECK(workspace_resources.work_units == 0);
  CHECK(workspace_resources.reservation_ordinal == 1);
}

TEST_CASE("profile refusal precedence is vector then live then work") {
  EvaluationResources all = make_bounded_resources(
      limits(7, 7, 7), no_allocation_failure);
  VectorAllocationResult vector_first = allocate_vector(
      all, ScalarType::integer, 1, 8, test_location, "iota");
  CHECK_FALSE(vector_first.ok);
  check_profile_refusal(vector_first.error,
                        ResourceLimitKind::max_vector_bytes, 7, 0, 8);

  EvaluationResources vector_live = make_bounded_resources(
      limits(7, 7, std::nullopt), no_allocation_failure);
  VectorAllocationResult vector_before_live = allocate_vector(
      vector_live, ScalarType::integer, 1, 0, test_location, "iota");
  CHECK_FALSE(vector_before_live.ok);
  check_profile_refusal(vector_before_live.error,
                        ResourceLimitKind::max_vector_bytes, 7, 0, 8);

  EvaluationResources vector_work = make_bounded_resources(
      limits(7, std::nullopt, 7), no_allocation_failure);
  VectorAllocationResult vector_before_work = allocate_vector(
      vector_work, ScalarType::integer, 1, 8, test_location, "iota");
  CHECK_FALSE(vector_before_work.ok);
  check_profile_refusal(vector_before_work.error,
                        ResourceLimitKind::max_vector_bytes, 7, 0, 8);

  EvaluationResources live_first = make_bounded_resources(
      limits(std::nullopt, 7, 7), no_allocation_failure);
  VectorAllocationResult live = allocate_vector(
      live_first, ScalarType::integer, 1, 8, test_location, "lifted-add");
  CHECK_FALSE(live.ok);
  check_profile_refusal(live.error,
                        ResourceLimitKind::max_live_evaluation_bytes, 7, 0, 8);

  EvaluationResources work_only = make_bounded_resources(
      limits(8, 8, 7), no_allocation_failure);
  VectorAllocationResult work = allocate_vector(
      work_only, ScalarType::integer, 1, 8, test_location, "lifted-add");
  CHECK_FALSE(work.ok);
  check_profile_refusal(work.error, ResourceLimitKind::max_work_units, 7, 0,
                        8);

  CHECK(all.live_evaluation_bytes == 0);
  CHECK(all.work_units == 0);
  CHECK(all.reservation_ordinal == 0);
  CHECK(vector_live.live_evaluation_bytes == 0);
  CHECK(vector_live.work_units == 0);
  CHECK(vector_live.reservation_ordinal == 0);
  CHECK(vector_work.live_evaluation_bytes == 0);
  CHECK(vector_work.work_units == 0);
  CHECK(vector_work.reservation_ordinal == 0);
  CHECK(live_first.live_evaluation_bytes == 0);
  CHECK(live_first.work_units == 0);
  CHECK(live_first.reservation_ordinal == 0);
  CHECK(work_only.live_evaluation_bytes == 0);
  CHECK(work_only.work_units == 0);
  CHECK(work_only.reservation_ordinal == 0);
}

TEST_CASE("generic workspace uses live bytes but not vector bytes") {
  EvaluationResources resources = make_bounded_resources(
      limits(0, 7, std::nullopt), no_allocation_failure);
  WorkspaceReservationResult refused = reserve_workspace(
      resources, 8, 0, test_location, "sort-workspace");

  CHECK_FALSE(refused.ok);
  check_profile_refusal(refused.error,
                        ResourceLimitKind::max_live_evaluation_bytes, 7, 0, 8);
  CHECK(resources.live_evaluation_bytes == 0);
  CHECK(resources.reservation_ordinal == 0);

  EvaluationResources exact = make_bounded_resources(
      limits(0, 8, std::nullopt), no_allocation_failure);
  WorkspaceReservationResult admitted = reserve_workspace(
      exact, 8, 0, test_location, "sort-workspace");
  REQUIRE(admitted.ok);
  CHECK(exact.live_evaluation_bytes == 8);
  CHECK(exact.reservation_ordinal == 1);
  release_workspace(exact, admitted.reservation);
  CHECK(exact.live_evaluation_bytes == 0);
  CHECK(admitted.reservation.storage.get() == nullptr);
  CHECK(admitted.reservation.bytes == 0);

  EvaluationResources injected = make_trusted_local_resources(
      AllocationFailureInjection{std::size_t{0}});
  WorkspaceReservationResult unavailable = reserve_workspace(
      injected, 1, 1, test_location, "sort-workspace");
  CHECK_FALSE(unavailable.ok);
  REQUIRE(unavailable.error.resource.has_value());
  CHECK(unavailable.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  CHECK(injected.live_evaluation_bytes == 0);
  CHECK(injected.work_units == 0);
  CHECK(injected.reservation_ordinal == 1);
}

TEST_CASE("work charging is monotonic exact and reset per context") {
  EvaluationResources first = make_bounded_resources(
      limits(std::nullopt, std::nullopt, 3), no_allocation_failure);
  REQUIRE(charge_work(first, 1, test_location, "inc").ok);
  REQUIRE(charge_work(first, 2, test_location, "iota").ok);
  CHECK(first.work_units == 3);

  const WorkChargeResult refused =
      charge_work(first, 1, test_location, "inc");
  CHECK_FALSE(refused.ok);
  check_profile_refusal(refused.error, ResourceLimitKind::max_work_units, 3, 3,
                        1);
  CHECK(first.work_units == 3);

  EvaluationResources second = make_bounded_resources(
      limits(std::nullopt, std::nullopt, 3), no_allocation_failure);
  REQUIRE(charge_work(second, 3, test_location, "iota").ok);
  CHECK(second.work_units == 3);
}

TEST_CASE("vector release refunds live bytes but not work") {
  EvaluationResources resources = make_bounded_resources(
      limits(8, 8, 2), no_allocation_failure);
  VectorAllocationResult allocated = allocate_vector(
      resources, ScalarType::integer, 1, 2, test_location, "lifted-inc");
  REQUIRE(allocated.ok);
  CHECK(resources.live_evaluation_bytes == 8);
  CHECK(resources.work_units == 2);

  release_vector_reservation(resources, allocated.value);
  CHECK(resources.live_evaluation_bytes == 0);
  CHECK(resources.work_units == 2);
  CHECK(allocated.value.container == ContainerKind::scalar);
  CHECK(validate_value(allocated.value).ok);

  Value malformed = make_int_value(0);
  malformed.container = ContainerKind::vector;
  malformed.vector.element_type = ScalarType::integer;
  malformed.vector.integer_count =
      std::numeric_limits<std::size_t>::max();
  resources.live_evaluation_bytes = 7;
  const ValueReleaseResult malformed_release =
      release_vector_reservation(resources, malformed);
  CHECK_FALSE(malformed_release.ok);
  CHECK(malformed_release.invariant ==
        ValueInvariant::inactive_scalar_field);
  CHECK(resources.live_evaluation_bytes == 7);
  CHECK(malformed.container == ContainerKind::vector);
  CHECK(malformed.vector.integer_count ==
        std::numeric_limits<std::size_t>::max());
}

TEST_CASE("trusted profile omits policy limits but retains mandatory safety") {
  EvaluationResources trusted =
      make_trusted_local_resources(no_allocation_failure);
  CHECK(trusted.profile == ExecutionProfile::trusted_local_v1);
  CHECK_FALSE(trusted.limits.max_vector_bytes.has_value());
  CHECK_FALSE(trusted.limits.max_live_evaluation_bytes.has_value());
  CHECK_FALSE(trusted.limits.max_work_units.has_value());

  const WorkChargeResult first =
      charge_work(trusted, 1, test_location, "inc");
  const WorkChargeResult second =
      charge_work(trusted, 2, test_location, "iota");
  REQUIRE(first.ok);
  REQUIRE(second.ok);
  CHECK(trusted.work_units == 3);
}

} // namespace bennu
