#ifndef BENNU_HOST_STORAGE_INTERNAL_HPP
#define BENNU_HOST_STORAGE_INTERNAL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>

namespace bennu::detail {

enum class SemanticHostAllocationPurpose : std::uint8_t {
  vector_payload,
  tuple_table,
  workspace,
};

using SemanticHostAllocationRelease =
    void (*)(std::uint64_t, std::size_t);

bool bind_semantic_host_buffer(
    const void *payload, const void *element_type,
    std::uint64_t owner_token, std::size_t canonical_bytes,
    std::size_t allocation_ordinal,
    SemanticHostAllocationPurpose purpose,
    SemanticHostAllocationRelease release);
bool semantic_host_buffer_matches(
    const void *payload, std::uint64_t owner_token,
    std::size_t canonical_bytes,
    std::optional<std::size_t> allocation_ordinal,
    SemanticHostAllocationPurpose purpose);
bool consume_semantic_host_buffer(
    const void *payload, std::uint64_t owner_token,
    std::size_t canonical_bytes,
    std::optional<std::size_t> allocation_ordinal,
    SemanticHostAllocationPurpose purpose);

} // namespace bennu::detail

#endif
