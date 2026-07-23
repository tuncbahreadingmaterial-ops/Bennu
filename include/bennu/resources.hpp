#ifndef BENNU_RESOURCES_HPP
#define BENNU_RESOURCES_HPP

#include "bennu/error.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace bennu {

enum class ExecutionProfile {
  trusted_local_v1,
  bounded_v1,
  trusted_local_v2,
  bounded_v2,
};

struct ResourceLimits {
  std::optional<std::size_t> max_vector_bytes;
  std::optional<std::size_t> max_live_evaluation_bytes;
  std::optional<std::size_t> max_work_units;
  std::optional<std::size_t> max_tuple_table_bytes{};
};

struct AllocationFailureInjection {
  std::optional<std::size_t> fail_at_reservation_ordinal;
};

struct EvaluationResources {
  ExecutionProfile profile;
  ResourceLimits limits;
  AllocationFailureInjection allocation_failure;
  std::shared_ptr<std::size_t> live_evaluation_accounting;
  std::size_t &live_evaluation_bytes;
  std::size_t work_units;
  std::size_t reservation_ordinal;

  EvaluationResources(ExecutionProfile profile, ResourceLimits limits,
                      AllocationFailureInjection allocation_failure,
                      std::size_t live_evaluation_bytes,
                      std::size_t work_units,
                      std::size_t reservation_ordinal);
  EvaluationResources(const EvaluationResources &other);
  EvaluationResources(EvaluationResources &&other) noexcept;
  EvaluationResources &operator=(const EvaluationResources &other) = delete;
  EvaluationResources &operator=(EvaluationResources &&other) = delete;
};

using WorkspaceStorage =
    std::unique_ptr<std::byte, decltype(&std::free)>;

struct WorkspaceReservation {
  WorkspaceStorage storage;
  std::size_t bytes;
};

struct VectorAllocationResult {
  bool ok;
  Value value;
  ValueInvariant invariant;
  Error error;
};

struct WorkspaceReservationResult {
  bool ok;
  WorkspaceReservation reservation;
  Error error;
};

struct WorkChargeResult {
  bool ok;
  Error error;
};

struct TupleReservationResult {
  bool ok;
  TupleTableReservation reservation;
  Error error;
};

struct TupleConstructionResult {
  bool ok;
  Value value;
  ValueInvariant invariant;
  Error error;
};

enum class ValueReleaseError {
  none,
  resource_context_mismatch,
};

struct ValueReleaseResult {
  bool ok;
  ValueInvariant invariant;
  ValueReleaseError error{ValueReleaseError::none};
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

EvaluationResources make_trusted_local_resources(
    AllocationFailureInjection allocation_failure);
EvaluationResources make_bounded_resources(
    ResourceLimits limits, AllocationFailureInjection allocation_failure);
EvaluationResources make_trusted_local_v2_resources(
    AllocationFailureInjection allocation_failure);
EvaluationResources make_bounded_v2_resources(
    ResourceLimits limits, AllocationFailureInjection allocation_failure);
std::string_view execution_profile_name(ExecutionProfile profile);

VectorAllocationResult allocate_vector(EvaluationResources &resources,
                                       ScalarType element_type,
                                       std::uint64_t requested_element_count,
                                       std::size_t work_units,
                                       SourceLocation location,
                                       std::string_view producer_name);
VectorAllocationResult copy_bool_vector(
    EvaluationResources &resources, std::span<const std::uint8_t> values,
    SourceLocation location, std::string_view producer_name);
VectorAllocationResult copy_int_vector(
    EvaluationResources &resources, std::span<const std::int64_t> values,
    SourceLocation location, std::string_view producer_name);
VectorAllocationResult copy_double_vector(
    EvaluationResources &resources, std::span<const double> values,
    SourceLocation location, std::string_view producer_name);
WorkspaceReservationResult reserve_workspace(
    EvaluationResources &resources, std::size_t byte_count,
    std::size_t work_units, SourceLocation location,
    std::string_view producer_name);
WorkChargeResult charge_work(EvaluationResources &resources,
                             std::size_t work_units,
                             SourceLocation location,
                             std::string_view producer_name);
TupleReservationResult reserve_tuple_table(
    EvaluationResources &resources, std::size_t element_count,
    SourceLocation location, std::string_view producer_name);
TupleConstructionResult make_tuple_value(
    EvaluationResources &resources, std::span<Value> elements,
    SourceLocation location, std::string_view producer_name);
TupleConstructionResult make_tuple_value(
    EvaluationResources &resources, std::span<Value> elements,
    SourceLocation location, std::string_view producer_name,
    HostAllocationFailureInjection &allocation_failure);
ValueReleaseResult release_vector_reservation(EvaluationResources &resources,
                                              Value &value);
ValueReleaseResult release_vector_reservation(
    EvaluationResources &resources, Value &value,
    HostAllocationFailureInjection &allocation_failure);
void release_workspace(EvaluationResources &resources,
                       WorkspaceReservation &reservation);
ValueReleaseResult release_value_reservations(EvaluationResources &resources,
                                              Value &value);
ValueReleaseResult release_value_reservations(
    EvaluationResources &resources, Value &value,
    HostAllocationFailureInjection &allocation_failure);

} // namespace bennu

#endif
