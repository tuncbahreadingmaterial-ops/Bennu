#include "bennu/resources.hpp"
#include "bennu/type.hpp"
#include "bennu/value.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr std::size_t large_child_count = 1024U * 1024U - 1U;
constexpr std::size_t refusal_headroom = 4U * 1024U * 1024U;

std::size_t current_virtual_bytes() {
  std::FILE *status = std::fopen("/proc/self/status", "r");
  if (status == nullptr) {
    return 0U;
  }
  char line[256]{};
  std::size_t virtual_kib = 0U;
  while (std::fgets(line, static_cast<int>(sizeof(line)), status) != nullptr) {
    if (std::strncmp(line, "VmSize:", 7U) == 0) {
      if (std::sscanf(line + 7, "%zu", &virtual_kib) != 1) {
        virtual_kib = 0U;
      }
      break;
    }
  }
  (void)std::fclose(status);
  return virtual_kib * 1024U;
}

bool impose_address_limit(std::size_t headroom) {
  const std::size_t virtual_bytes = current_virtual_bytes();
  if (virtual_bytes == 0U ||
      virtual_bytes > static_cast<std::size_t>(RLIM_INFINITY) - headroom) {
    return false;
  }
  const rlim_t address_limit =
      static_cast<rlim_t>(virtual_bytes + headroom);
  const rlimit limit{address_limit, address_limit};
  return setrlimit(RLIMIT_AS, &limit) == 0;
}

bool make_large_type(bennu::TypeArena &arena) {
  if (bennu::allocate_host_array(arena.nodes, large_child_count + 1U,
                                 std::nullopt) !=
          bennu::HostResourceErrorReason::none ||
      bennu::allocate_host_array(arena.child_indexes, large_child_count,
                                 std::nullopt) !=
          bennu::HostResourceErrorReason::none) {
    return false;
  }
  for (std::size_t index = 0U; index < large_child_count; ++index) {
    if (bennu::host_array_push(
            arena.nodes,
            bennu::TypeNode{bennu::TypeKind::scalar,
                            bennu::ScalarType::integer, 0U, 0U}) !=
            bennu::HostResourceErrorReason::none ||
        bennu::host_array_push(arena.child_indexes, index) !=
            bennu::HostResourceErrorReason::none) {
      return false;
    }
  }
  if (bennu::host_array_push(
          arena.nodes,
          bennu::TypeNode{bennu::TypeKind::tuple,
                          bennu::ScalarType::boolean, 0U,
                          large_child_count}) !=
      bennu::HostResourceErrorReason::none) {
    return false;
  }
  arena.root_index = large_child_count;
  return bennu::validate_type(arena).ok;
}

bool make_large_value(bennu::Value &value) {
  value = bennu::make_int_value(0);
  value.container = bennu::ContainerKind::tuple;
  value.scalar =
      bennu::ScalarValue{bennu::ScalarType::boolean, false, 0, 0.0};
  if (bennu::allocate_host_array(value.tuple.nodes, large_child_count,
                                 std::nullopt) !=
          bennu::HostResourceErrorReason::none ||
      bennu::allocate_host_array(value.tuple.child_indexes,
                                 large_child_count, std::nullopt) !=
          bennu::HostResourceErrorReason::none) {
    return false;
  }
  for (std::size_t index = 0U; index < large_child_count; ++index) {
    if (bennu::host_array_push(
            value.tuple.nodes,
            bennu::ValueNode{
                bennu::ContainerKind::scalar,
                bennu::ScalarValue{
                    bennu::ScalarType::integer, false,
                    static_cast<std::int64_t>(index), 0.0},
                0U, 0U, 0U, 0U}) !=
            bennu::HostResourceErrorReason::none ||
        bennu::host_array_push(value.tuple.child_indexes, index) !=
            bennu::HostResourceErrorReason::none) {
      return false;
    }
  }
  value.tuple.root_index = large_child_count;
  value.tuple.first_child = 0U;
  value.tuple.child_count = large_child_count;
  const std::size_t reservation_bytes = large_child_count * 16U;
  bennu::TupleTableStorage reservation{
      nullptr, &bennu::release_host_buffer<std::byte>};
  if (bennu::allocate_host_buffer(reservation, reservation_bytes) !=
      bennu::HostResourceErrorReason::none) {
    return false;
  }
  value.tuple.root_reservation.storage = std::move(reservation);
  value.tuple.root_reservation.element_count = large_child_count;
  value.tuple.root_reservation.canonical_bytes = reservation_bytes;
  return bennu::validate_value(value).ok;
}

bool refused(bennu::HostResourceErrorReason reason) {
  return reason == bennu::HostResourceErrorReason::allocation_unavailable;
}

int type_validation_refusal() {
  bennu::TypeArena type;
  if (!make_large_type(type) || !impose_address_limit(refusal_headroom)) {
    return 20;
  }
  const bennu::TypeValidationResult result = bennu::validate_type(type);
  return !result.ok && refused(result.resource_error) ? 0 : 21;
}

int type_construction_refusal() {
  std::array<bennu::TypeArena, 1U> elements{};
  if (!make_large_type(elements[0]) ||
      !impose_address_limit(refusal_headroom)) {
    return 22;
  }
  const bennu::TypeConstructionResult result =
      bennu::make_tuple_type(elements);
  return !result.ok && refused(result.resource_error) ? 0 : 23;
}

int type_equality_refusal() {
  bennu::TypeArena type;
  if (!make_large_type(type) || !impose_address_limit(refusal_headroom)) {
    return 24;
  }
  bennu::HostAllocationFailureInjection failure{std::nullopt, 0U};
  const bennu::TypeEqualityResult result =
      bennu::structural_type_equal(type, type, failure);
  return !result.ok && refused(result.resource_error) ? 0 : 25;
}

int type_formatting_refusal() {
  bennu::TypeArena type;
  if (!make_large_type(type) || !impose_address_limit(refusal_headroom)) {
    return 26;
  }
  const bennu::TypeFormattingResult result = bennu::format_type(type);
  return !result.ok && refused(result.resource_error) ? 0 : 27;
}

int value_validation_refusal() {
  bennu::Value value = bennu::make_int_value(0);
  if (!make_large_value(value) || !impose_address_limit(refusal_headroom)) {
    return 28;
  }
  const bennu::ValueValidationResult result = bennu::validate_value(value);
  return !result.ok && refused(result.resource_error) ? 0 : 29;
}

int value_type_refusal() {
  bennu::Value value = bennu::make_int_value(0);
  if (!make_large_value(value) || !impose_address_limit(refusal_headroom)) {
    return 30;
  }
  const bennu::ValueTypeResult result = bennu::value_type(value);
  return !result.ok && refused(result.resource_error) ? 0 : 31;
}

int value_formatting_refusal() {
  bennu::Value value = bennu::make_int_value(0);
  if (!make_large_value(value) || !impose_address_limit(refusal_headroom)) {
    return 32;
  }
  const bennu::ValueFormattingResult result = bennu::format_value(value);
  return !result.ok && refused(result.resource_error) ? 0 : 33;
}

int value_destruction_refusal() {
  bennu::Value value = bennu::make_int_value(0);
  if (!make_large_value(value) || !impose_address_limit(refusal_headroom)) {
    return 34;
  }
  const bennu::ValueNode *nodes_before = value.tuple.nodes.storage.get();
  const bennu::ValueDestructionResult result = bennu::destroy_value(value);
  return !result.ok && refused(result.resource_error) && value.claimed &&
                 value.tuple.nodes.storage.get() == nodes_before
             ? 0
             : 35;
}

int value_release_refusal() {
  bennu::Value value = bennu::make_int_value(0);
  bennu::EvaluationResources resources =
      bennu::make_trusted_local_v2_resources(
          bennu::AllocationFailureInjection{std::nullopt});
  if (!make_large_value(value) || !impose_address_limit(refusal_headroom)) {
    return 36;
  }
  const bennu::ValueNode *nodes_before = value.tuple.nodes.storage.get();
  const std::size_t live_before = resources.live_evaluation_bytes;
  const bennu::ValueReleaseResult result =
      bennu::release_value_reservations(resources, value);
  return !result.ok && refused(result.resource_error) && value.claimed &&
                 value.tuple.nodes.storage.get() == nodes_before &&
                 resources.live_evaluation_bytes == live_before
             ? 0
             : 37;
}

int value_detach_refusal() {
  bennu::Value value = bennu::make_int_value(0);
  bennu::EvaluationResources resources =
      bennu::make_trusted_local_v2_resources(
          bennu::AllocationFailureInjection{std::nullopt});
  if (!make_large_value(value) || !impose_address_limit(refusal_headroom)) {
    return 38;
  }
  const bennu::ValueNode *nodes_before = value.tuple.nodes.storage.get();
  const std::size_t live_before = resources.live_evaluation_bytes;
  const bennu::ValueReleaseResult result =
      bennu::detach_value_reservations(resources, value);
  return !result.ok && refused(result.resource_error) && value.claimed &&
                 value.tuple.nodes.storage.get() == nodes_before &&
                 resources.live_evaluation_bytes == live_before
             ? 0
             : 39;
}

int tuple_metadata_refusal() {
  constexpr std::size_t element_count = 256U * 1024U;
  bennu::HostArray<bennu::Value> elements;
  if (bennu::allocate_host_array(elements, element_count, std::nullopt) !=
      bennu::HostResourceErrorReason::none) {
    return 40;
  }
  for (std::size_t index = 0U; index < element_count; ++index) {
    if (bennu::host_array_push(
            elements,
            bennu::make_int_value(static_cast<std::int64_t>(index))) !=
        bennu::HostResourceErrorReason::none) {
      return 43;
    }
  }
  bennu::EvaluationResources resources =
      bennu::make_trusted_local_v2_resources(
          bennu::AllocationFailureInjection{std::nullopt});
  if (!impose_address_limit(8U * 1024U * 1024U)) {
    return 41;
  }
  bennu::HostAllocationFailureInjection failure{std::nullopt, 0U};
  bennu::TupleConstructionResult result = bennu::make_tuple_value(
      resources, bennu::host_array_span(elements), bennu::SourceLocation{},
      "actual-metadata-refusal", failure);
  bool ownership_preserved = true;
  for (const bennu::Value &element : bennu::host_array_span(elements)) {
    ownership_preserved = ownership_preserved && element.claimed;
  }
  const bool explicit_refusal =
      !result.ok && result.error.resource.has_value() &&
      result.error.resource->reason ==
          bennu::ResourceErrorReason::allocation_unavailable;
  return explicit_refusal && ownership_preserved &&
                 resources.live_evaluation_bytes == 0U &&
                 resources.reservation_ordinal == 1U &&
                 failure.allocation_ordinal == 1U
             ? 0
             : 42;
}

int resource_creation_refusal() {
  constexpr std::size_t context_capacity = 65536U;
  void *context_storage =
      std::malloc(sizeof(bennu::EvaluationResources) *
                  context_capacity);
  if (context_storage == nullptr) {
    return 44;
  }
  if (!impose_address_limit(256U * 1024U)) {
    std::free(context_storage);
    return 44;
  }
  for (std::size_t index = 0U; index < 4096U; ++index) {
    bennu::EvaluationResources resources =
        bennu::make_trusted_local_v2_resources(
            bennu::AllocationFailureInjection{std::nullopt});
    if (resources.creation_error ==
        bennu::HostResourceErrorReason::allocation_unavailable) {
      std::free(context_storage);
      return 45;
    }
  }
  auto *contexts =
      static_cast<bennu::EvaluationResources *>(context_storage);
  std::size_t constructed = 0U;
  for (; constructed < context_capacity; ++constructed) {
    bennu::EvaluationResources resources =
        bennu::make_trusted_local_v2_resources(
            bennu::AllocationFailureInjection{std::nullopt});
    if (resources.creation_error ==
        bennu::HostResourceErrorReason::allocation_unavailable) {
      const bennu::WorkChargeResult refused = bennu::charge_work(
          resources, 1U, bennu::SourceLocation{},
          "resource-create-refusal");
      const bool explicit_refusal =
          !refused.ok && refused.error.resource.has_value() &&
          refused.error.resource->reason ==
              bennu::ResourceErrorReason::allocation_unavailable;
      for (std::size_t index = constructed; index > 0U; --index) {
        std::destroy_at(contexts + index - 1U);
      }
      std::free(context_storage);
      return explicit_refusal ? 0 : 46;
    }
    std::construct_at(contexts + constructed,
                      std::move(resources));
  }
  for (std::size_t index = constructed; index > 0U; --index) {
    std::destroy_at(contexts + index - 1U);
  }
  std::free(context_storage);
  return 46;
}

int profile_and_coordinator_refusal() {
  bennu::EvaluationResources v1 =
      bennu::make_trusted_local_resources(
          bennu::AllocationFailureInjection{std::nullopt});
  bennu::EvaluationResources invalid =
      bennu::make_trusted_local_v2_resources(
          bennu::AllocationFailureInjection{std::nullopt});
  bennu::EvaluationResources coordinator =
      bennu::make_trusted_local_v2_resources(
          bennu::AllocationFailureInjection{std::nullopt});
  if (!impose_address_limit(64U * 1024U)) {
    return 54;
  }

  std::array<bennu::Value, 0U> empty{};
  const bennu::TupleConstructionResult unsupported =
      bennu::make_tuple_value(v1, empty, bennu::SourceLocation{},
                              "v1-profile-refusal");
  if (unsupported.ok ||
      unsupported.error.kind != bennu::ErrorKind::profile_error ||
      !unsupported.error.profile.has_value() ||
      unsupported.error.profile->profile_name != "trusted-local-v1") {
    return 55;
  }

  invalid.profile = static_cast<bennu::ExecutionProfile>(999U);
  const bennu::WorkChargeResult invalid_profile =
      bennu::charge_work(invalid, 1U, bennu::SourceLocation{},
                         "invalid-profile-refusal");
  if (invalid_profile.ok ||
      invalid_profile.error.kind !=
          bennu::ErrorKind::invalid_execution_profile ||
      invalid_profile.error.static_message.empty()) {
    return 56;
  }

  const bennu::TupleConstructionResult null_executor =
      bennu::execute_tuple_construction(
          coordinator, empty, nullptr, nullptr,
          bennu::SourceLocation{}, "null-coordinator");
  return !null_executor.ok &&
                 null_executor.error.kind ==
                     bennu::ErrorKind::domain_error &&
                 !null_executor.error.static_message.empty()
             ? 0
             : 57;
}

int malformed_pointer_refusal() {
  bennu::Value forged_vector = bennu::make_int_value(0);
  forged_vector.container = bennu::ContainerKind::vector;
  forged_vector.scalar =
      bennu::ScalarValue{bennu::ScalarType::boolean, false, 0, 0.0};
  forged_vector.vector.element_type = bennu::ScalarType::integer;
  forged_vector.vector.integers.reset(
      reinterpret_cast<std::int64_t *>(
          static_cast<std::uintptr_t>(0x1000U)));
  forged_vector.vector.integer_count = 1U;
  forged_vector.vector.canonical_bytes = sizeof(std::int64_t);
  if (bennu::validate_value(forged_vector).invariant !=
      bennu::ValueInvariant::invalid_vector_payload_handle) {
    return 47;
  }

  bennu::Value forged_array = bennu::make_int_value(0);
  forged_array.container = bennu::ContainerKind::tuple;
  forged_array.scalar =
      bennu::ScalarValue{bennu::ScalarType::boolean, false, 0, 0.0};
  forged_array.tuple.nodes.storage.reset(
      reinterpret_cast<bennu::ValueNode *>(
          static_cast<std::uintptr_t>(0x2000U)));
  forged_array.tuple.nodes.size = 1U;
  forged_array.tuple.nodes.capacity = 1U;
  return !bennu::validate_value(forged_array).ok ? 0 : 48;
}

struct NestedCleanupContext {};

bennu::TupleElementExecutionResult execute_nested_cleanup_element(
    void *, bennu::EvaluationResources &resources,
    std::size_t element_index) {
  if (element_index != 0U) {
    return bennu::TupleElementExecutionResult{
        true,
        bennu::make_int_value(static_cast<std::int64_t>(element_index)),
        bennu::make_error(bennu::ErrorKind::none,
                          bennu::SourceLocation{})};
  }
  bennu::VectorAllocationResult vector = bennu::allocate_vector(
      resources, bennu::ScalarType::integer, 1U, 0U,
      bennu::SourceLocation{}, "nested-cleanup-child");
  if (!vector.ok) {
    return bennu::TupleElementExecutionResult{
        false, bennu::make_int_value(0), std::move(vector.error)};
  }
  std::array<bennu::Value, 1U> nested_child{{
      bennu::move_value(vector.value),
  }};
  bennu::TupleConstructionResult nested = bennu::make_tuple_value(
      resources, nested_child, bennu::SourceLocation{},
      "nested-cleanup-child");
  if (!nested.ok) {
    return bennu::TupleElementExecutionResult{
        false, bennu::make_int_value(0), std::move(nested.error)};
  }
  return bennu::TupleElementExecutionResult{
      true, bennu::move_value(nested.value),
      bennu::make_error(bennu::ErrorKind::none,
                        bennu::SourceLocation{})};
}

int nested_construction_cleanup_refusal() {
  constexpr std::size_t element_count = 128U * 1024U;
  bennu::HostArray<bennu::Value> scratch;
  if (bennu::allocate_host_array(scratch, element_count, std::nullopt) !=
      bennu::HostResourceErrorReason::none) {
    return 49;
  }
  for (std::size_t index = 0U; index < element_count; ++index) {
    if (bennu::host_array_push(scratch, bennu::make_int_value(0)) !=
        bennu::HostResourceErrorReason::none) {
      return 50;
    }
  }
  bennu::EvaluationResources resources =
      bennu::make_trusted_local_v2_resources(
          bennu::AllocationFailureInjection{std::nullopt});
  if (!impose_address_limit(4U * 1024U * 1024U)) {
    return 51;
  }
  NestedCleanupContext context;
  const bennu::TupleConstructionResult result =
      bennu::execute_tuple_construction(
          resources, bennu::host_array_span(scratch),
          &execute_nested_cleanup_element, &context,
          bennu::SourceLocation{}, "real-nested-cleanup");
  if (result.ok || !result.error.resource.has_value() ||
      result.error.resource->reason !=
          bennu::ResourceErrorReason::allocation_unavailable ||
      resources.live_evaluation_bytes != 0U) {
    return 52;
  }
  for (const bennu::Value &slot : bennu::host_array_span(scratch)) {
    if (slot.claimed) {
      return 53;
    }
  }
  return 0;
}

using RefusalProbe = int (*)();

struct RefusalCase {
  const char *name;
  RefusalProbe probe;
};

int run_isolated(RefusalProbe probe) {
  const pid_t child = fork();
  if (child < 0) {
    return 10;
  }
  if (child == 0) {
    _exit(probe());
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    return 11;
  }
  if (!WIFEXITED(status)) {
    return 12;
  }
  return WEXITSTATUS(status);
}

} // namespace

int main(int argument_count, char **arguments) {
  if (argument_count == 2 &&
      std::strcmp(arguments[1], "--malformed-pointers") == 0) {
    return run_isolated(&malformed_pointer_refusal);
  }
  constexpr std::array<RefusalCase, 15U> probes{{
      {"type validation", &type_validation_refusal},
      {"type construction", &type_construction_refusal},
      {"type equality", &type_equality_refusal},
      {"type formatting", &type_formatting_refusal},
      {"value validation", &value_validation_refusal},
      {"value type", &value_type_refusal},
      {"value formatting", &value_formatting_refusal},
      {"value destruction", &value_destruction_refusal},
      {"value release", &value_release_refusal},
      {"value detach", &value_detach_refusal},
      {"tuple metadata", &tuple_metadata_refusal},
      {"resource creation", &resource_creation_refusal},
      {"profile and coordinator", &profile_and_coordinator_refusal},
      {"malformed pointers", &malformed_pointer_refusal},
      {"nested construction cleanup", &nested_construction_cleanup_refusal},
  }};
  for (const RefusalCase &probe : probes) {
    const int result = run_isolated(probe.probe);
    if (result != 0) {
      std::fprintf(stderr, "real allocator refusal probe '%s' failed: %d\n",
                   probe.name, result);
      return result;
    }
  }
  return 0;
}
