#include "bennu/resources.hpp"

#include "doctest/doctest.h"

#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace bennu {

namespace {

constexpr std::size_t bool_payload_width = 1;
constexpr std::size_t int_payload_width = 8;
constexpr std::size_t double_payload_width = 8;
static_assert(sizeof(std::uint8_t) == bool_payload_width);
static_assert(sizeof(std::int64_t) == int_payload_width);
static_assert(sizeof(double) == double_payload_width);

Error no_error() {
  return make_error(ErrorKind::none, SourceLocation{0, 1, 1});
}

std::string_view profile_name(ExecutionProfile profile) {
  switch (profile) {
  case ExecutionProfile::trusted_local_v1:
    return "trusted-local-v1";
  case ExecutionProfile::bounded_v1:
    return "bounded-v1";
  }
  return "";
}

Value empty_vector(ScalarType element_type) {
  return Value{
      ContainerKind::vector,
      ScalarValue{ScalarType::boolean, false, 0, 0.0},
      VectorValue{element_type,
                  {nullptr, &std::free},
                  0,
                  {nullptr, &std::free},
                  0,
                  {nullptr, &std::free},
                  0},
  };
}

WorkspaceReservation empty_workspace() {
  return WorkspaceReservation{{nullptr, &std::free}, 0};
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

Error make_resource_failure(
    EvaluationResources &resources, ResourceErrorReason reason,
    SourceLocation location, std::string_view producer_name,
    std::optional<std::size_t> requested_elements,
    std::optional<std::size_t> requested_bytes,
    std::optional<ResourceLimitKind> limit_kind,
    std::optional<std::size_t> configured_limit,
    std::optional<std::size_t> usage_before,
    std::optional<std::size_t> refused_charge) {
  Error error = make_error(ErrorKind::resource_error, location);
  if (!producer_name.empty()) {
    error.primitive = PrimitiveErrorContext{std::string(producer_name)};
  }
  error.resource = ResourceErrorContext{
      reason,
      requested_elements,
      requested_bytes,
      std::string(profile_name(resources.profile)),
      limit_kind,
      configured_limit,
      usage_before,
      refused_charge,
  };
  return error;
}

std::optional<Error>
validate_profile_configuration(const EvaluationResources &resources,
                               SourceLocation location,
                               std::string_view producer_name) {
  const bool has_configured_limit =
      resources.limits.max_vector_bytes.has_value() ||
      resources.limits.max_live_evaluation_bytes.has_value() ||
      resources.limits.max_work_units.has_value();
  std::string_view message;
  switch (resources.profile) {
  case ExecutionProfile::trusted_local_v1:
    if (!has_configured_limit) {
      return std::nullopt;
    }
    message = "trusted-local-v1 requires every resource limit to be omitted";
    break;
  case ExecutionProfile::bounded_v1:
    if (has_configured_limit) {
      return std::nullopt;
    }
    message = "bounded-v1 requires at least one configured resource limit";
    break;
  default:
    message = "execution profile tag is unknown";
    break;
  }

  Error error =
      make_error(ErrorKind::invalid_execution_profile, location,
                 std::string(message));
  if (!producer_name.empty()) {
    error.primitive = PrimitiveErrorContext{std::string(producer_name)};
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

EvaluationResources make_trusted_local_resources(
    AllocationFailureInjection allocation_failure) {
  return EvaluationResources{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      allocation_failure,
      0,
      0,
      0,
  };
}

EvaluationResources make_bounded_resources(
    ResourceLimits limits, AllocationFailureInjection allocation_failure) {
  return EvaluationResources{ExecutionProfile::bounded_v1,
                             limits,
                             allocation_failure,
                             0,
                             0,
                             0};
}

VectorAllocationResult allocate_vector(EvaluationResources &resources,
                                       ScalarType element_type,
                                       std::uint64_t requested_element_count,
                                       std::size_t work_units,
                                       SourceLocation location,
                                       std::string_view producer_name) {
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
            std::nullopt, std::nullopt, std::nullopt));
  }

  void *storage = std::malloc(byte_count);
  if (storage == nullptr) {
    return allocation_failure(
        std::move(candidate),
        make_resource_failure(
            resources, ResourceErrorReason::allocation_unavailable, location,
            producer_name, element_count, byte_count, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt));
  }
  std::memset(storage, 0, byte_count);
  switch (element_type) {
  case ScalarType::boolean:
    candidate.vector.booleans.reset(
        static_cast<std::uint8_t *>(storage));
    candidate.vector.boolean_count = element_count;
    break;
  case ScalarType::integer:
    candidate.vector.integers.reset(
        static_cast<std::int64_t *>(storage));
    candidate.vector.integer_count = element_count;
    break;
  case ScalarType::double_precision:
    candidate.vector.doubles.reset(static_cast<double *>(storage));
    candidate.vector.double_count = element_count;
    break;
  }
  commit_admission(resources, admission);
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
            std::nullopt, std::nullopt),
    };
  }
  void *storage = std::malloc(byte_count);
  if (storage == nullptr) {
    return WorkspaceReservationResult{
        false,
        empty_workspace(),
        make_resource_failure(
            resources, ResourceErrorReason::allocation_unavailable, location,
            producer_name, std::nullopt, byte_count, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt),
    };
  }
  commit_admission(resources, admission);
  return WorkspaceReservationResult{
      true,
      WorkspaceReservation{WorkspaceStorage{static_cast<std::byte *>(storage),
                                            &std::free},
                           byte_count},
      no_error(),
  };
}

WorkChargeResult charge_work(EvaluationResources &resources,
                             std::size_t work_units,
                             SourceLocation location,
                             std::string_view producer_name) {
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

void release_vector_reservation(EvaluationResources &resources, Value &value) {
  if (value.container != ContainerKind::vector) {
    return;
  }
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
    if (bytes <= resources.live_evaluation_bytes) {
      resources.live_evaluation_bytes -= bytes;
    }
  }
  destroy_value(value);
}

void release_workspace(EvaluationResources &resources,
                       WorkspaceReservation &reservation) {
  if (reservation.bytes <= resources.live_evaluation_bytes) {
    resources.live_evaluation_bytes -= reservation.bytes;
  }
  reservation.storage.reset();
  reservation.bytes = 0;
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
  CHECK(error.message == std::string(expected_message));
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
  release_vector_reservation(resources, malformed);
  CHECK(resources.live_evaluation_bytes == 7);
  CHECK(malformed.container == ContainerKind::scalar);
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
