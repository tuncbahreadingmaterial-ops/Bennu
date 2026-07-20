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
};

struct ResourceLimits {
  std::optional<std::size_t> max_vector_bytes;
  std::optional<std::size_t> max_live_evaluation_bytes;
  std::optional<std::size_t> max_work_units;
};

struct AllocationFailureInjection {
  std::optional<std::size_t> fail_at_reservation_ordinal;
};

struct EvaluationResources {
  ExecutionProfile profile;
  ResourceLimits limits;
  AllocationFailureInjection allocation_failure;
  std::size_t live_evaluation_bytes;
  std::size_t work_units;
  std::size_t reservation_ordinal;
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

EvaluationResources make_trusted_local_resources(
    AllocationFailureInjection allocation_failure);
EvaluationResources make_bounded_resources(
    ResourceLimits limits, AllocationFailureInjection allocation_failure);

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
void release_vector_reservation(EvaluationResources &resources,
                                Value &value);
void release_workspace(EvaluationResources &resources,
                       WorkspaceReservation &reservation);

} // namespace bennu

#endif
