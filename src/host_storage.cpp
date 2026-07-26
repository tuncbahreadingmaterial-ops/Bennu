#include "bennu/host_storage.hpp"

#include "host_storage_internal.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <new>

namespace bennu {
namespace {

struct HostArrayAllocationMetadata {
  std::size_t constructed;
  std::size_t capacity;
};

struct SemanticHostAllocationMetadata {
  std::uint64_t owner_token;
  std::size_t canonical_bytes;
  std::size_t allocation_ordinal;
  detail::SemanticHostAllocationPurpose purpose;
  detail::SemanticHostAllocationRelease release;
  bool active;
};

union HostAllocationMetadata {
  HostArrayAllocationMetadata array;
  SemanticHostAllocationMetadata semantic;
};

struct HostAllocationRecord {
  HostAllocationRecord *next;
  void *allocation;
  void *payload;
  std::size_t byte_count;
  HostAllocationPurpose purpose;
  const void *element_type;
  HostAllocationMetadata metadata;
};

constexpr std::size_t host_allocation_bucket_count = 4096U;

struct HostAllocationBucket {
  HostAllocationRecord *records;
  std::mutex mutex;
};

std::array<HostAllocationBucket, host_allocation_bucket_count>
    host_allocation_buckets{};
std::atomic<std::size_t> registered_host_allocations{0U};

std::size_t host_allocation_bucket_index(const void *payload) {
  const std::uintptr_t address =
      reinterpret_cast<std::uintptr_t>(payload);
  return ((address >> 4U) ^ (address >> 17U)) &
         (host_allocation_bucket_count - 1U);
}

HostAllocationRecord *find_host_allocation_unlocked(
    HostAllocationBucket &bucket, const void *payload,
    HostAllocationPurpose purpose, const void *element_type) {
  for (HostAllocationRecord *record = bucket.records;
       record != nullptr; record = record->next) {
    if (record->payload == payload && record->purpose == purpose &&
        record->element_type == element_type) {
      return record;
    }
  }
  return nullptr;
}

HostAllocationSnapshot snapshot(const HostAllocationRecord *record) {
  if (record == nullptr) {
    return HostAllocationSnapshot{false, nullptr, 0U, 0U, 0U};
  }
  return HostAllocationSnapshot{
      true, record->allocation, record->byte_count,
      record->purpose == HostAllocationPurpose::array
          ? record->metadata.array.constructed
          : 0U,
      record->purpose == HostAllocationPurpose::array
          ? record->metadata.array.capacity
          : 0U};
}

} // namespace

namespace detail {

std::size_t host_allocation_header_size() {
  return sizeof(HostAllocationRecord);
}

std::size_t host_allocation_header_alignment() {
  return alignof(HostAllocationRecord);
}

bool register_host_allocation(
    void *allocation, void *payload, std::size_t byte_count,
    HostAllocationPurpose purpose, const void *element_type,
    std::size_t constructed, std::size_t capacity) {
  if (allocation == nullptr || payload == nullptr ||
      element_type == nullptr) {
    return false;
  }
  HostAllocationMetadata metadata{};
  if (purpose == HostAllocationPurpose::array) {
    metadata.array =
        HostArrayAllocationMetadata{constructed, capacity};
  } else {
    metadata.semantic = SemanticHostAllocationMetadata{
        0U, 0U, 0U, SemanticHostAllocationPurpose::workspace,
        nullptr, false};
  }
  auto *record = new (allocation) HostAllocationRecord{
      nullptr, allocation, payload, byte_count, purpose,
      element_type, metadata};
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  if (find_host_allocation_unlocked(
          bucket, payload, purpose, element_type) != nullptr) {
    std::destroy_at(record);
    return false;
  }
  record->next = bucket.records;
  bucket.records = record;
  registered_host_allocations.fetch_add(1U, std::memory_order_relaxed);
  return true;
}

std::size_t registered_host_allocation_count() {
  return registered_host_allocations.load(std::memory_order_relaxed);
}

HostAllocationSnapshot find_host_allocation(
    const void *payload, HostAllocationPurpose purpose,
    const void *element_type) {
  if (payload == nullptr || element_type == nullptr) {
    return snapshot(nullptr);
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  return snapshot(find_host_allocation_unlocked(
      bucket, payload, purpose, element_type));
}

HostAllocationSnapshot remove_host_allocation(
    const void *payload, HostAllocationPurpose purpose,
    const void *element_type) {
  if (payload == nullptr || element_type == nullptr) {
    return snapshot(nullptr);
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  HostAllocationRecord *removed = nullptr;
  {
    const std::lock_guard<std::mutex> lock(bucket.mutex);
    HostAllocationRecord **link = &bucket.records;
    while (*link != nullptr) {
      HostAllocationRecord *record = *link;
      if (record->payload == payload && record->purpose == purpose &&
          record->element_type == element_type) {
        *link = record->next;
        record->next = nullptr;
        removed = record;
        break;
      }
      link = &record->next;
    }
  }
  const HostAllocationSnapshot result = snapshot(removed);
  if (removed != nullptr &&
      removed->purpose == HostAllocationPurpose::buffer &&
      removed->metadata.semantic.active &&
      removed->metadata.semantic.release != nullptr) {
    removed->metadata.semantic.active = false;
    removed->metadata.semantic.release(
        removed->metadata.semantic.owner_token,
        removed->metadata.semantic.canonical_bytes);
  }
  if (removed != nullptr) {
    std::destroy_at(removed);
  }
  return result;
}

bool claim_host_array_elements(
    const void *payload, const void *element_type,
    std::size_t expected_capacity, std::size_t current_size,
    std::size_t count) {
  if (payload == nullptr || element_type == nullptr) {
    return false;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  HostAllocationRecord *record = find_host_allocation_unlocked(
      bucket, payload, HostAllocationPurpose::array, element_type);
  if (record == nullptr ||
      record->metadata.array.capacity != expected_capacity ||
      current_size > record->metadata.array.constructed ||
      count > record->metadata.array.capacity - current_size) {
    return false;
  }
  const std::size_t claimed = current_size + count;
  if (claimed > record->metadata.array.constructed) {
    record->metadata.array.constructed = claimed;
  }
  return true;
}

bool bind_semantic_host_buffer(
    const void *payload, const void *element_type,
    std::uint64_t owner_token, std::size_t canonical_bytes,
    std::size_t allocation_ordinal,
    SemanticHostAllocationPurpose purpose,
    SemanticHostAllocationRelease release) {
  if (payload == nullptr || element_type == nullptr ||
      owner_token == 0U || release == nullptr) {
    return false;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  HostAllocationRecord *record = find_host_allocation_unlocked(
      bucket, payload, HostAllocationPurpose::buffer, element_type);
  if (record == nullptr || record->metadata.semantic.active) {
    return false;
  }
  record->metadata.semantic.owner_token = owner_token;
  record->metadata.semantic.canonical_bytes = canonical_bytes;
  record->metadata.semantic.allocation_ordinal = allocation_ordinal;
  record->metadata.semantic.purpose = purpose;
  record->metadata.semantic.release = release;
  record->metadata.semantic.active = true;
  return true;
}

bool semantic_host_buffer_matches(
    const void *payload, std::uint64_t owner_token,
    std::size_t canonical_bytes,
    std::optional<std::size_t> allocation_ordinal,
    SemanticHostAllocationPurpose purpose) {
  if (payload == nullptr || owner_token == 0U ||
      !allocation_ordinal.has_value()) {
    return false;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  for (const HostAllocationRecord *record = bucket.records;
       record != nullptr; record = record->next) {
    if (record->payload == payload &&
        record->purpose == HostAllocationPurpose::buffer) {
      return record->metadata.semantic.active &&
             record->metadata.semantic.owner_token == owner_token &&
             record->metadata.semantic.canonical_bytes ==
                 canonical_bytes &&
             record->metadata.semantic.allocation_ordinal ==
                 *allocation_ordinal &&
             record->metadata.semantic.purpose == purpose;
    }
  }
  return false;
}

bool consume_semantic_host_buffer(
    const void *payload, std::uint64_t owner_token,
    std::size_t canonical_bytes,
    std::optional<std::size_t> allocation_ordinal,
    SemanticHostAllocationPurpose purpose) {
  if (payload == nullptr || owner_token == 0U ||
      !allocation_ordinal.has_value()) {
    return false;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  for (HostAllocationRecord *record = bucket.records;
       record != nullptr; record = record->next) {
    if (record->payload != payload ||
        record->purpose != HostAllocationPurpose::buffer) {
      continue;
    }
    if (!record->metadata.semantic.active ||
        record->metadata.semantic.owner_token != owner_token ||
        record->metadata.semantic.canonical_bytes != canonical_bytes ||
        record->metadata.semantic.allocation_ordinal !=
            *allocation_ordinal ||
        record->metadata.semantic.purpose != purpose) {
      return false;
    }
    record->metadata.semantic.active = false;
    return true;
  }
  return false;
}

} // namespace detail
} // namespace bennu
