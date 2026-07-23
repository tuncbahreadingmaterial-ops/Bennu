#include "bennu/resources.hpp"
#include "bennu/type.hpp"
#include "bennu/value.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  void *reservation = std::malloc(reservation_bytes);
  if (reservation == nullptr) {
    return false;
  }
  value.tuple.root_reservation.storage.reset(
      static_cast<std::byte *>(reservation));
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

int main() {
  constexpr std::array<RefusalCase, 11U> probes{{
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
