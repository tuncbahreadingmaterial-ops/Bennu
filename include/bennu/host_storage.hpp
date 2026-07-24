#ifndef BENNU_HOST_STORAGE_HPP
#define BENNU_HOST_STORAGE_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace bennu {

enum class HostResourceErrorReason {
  none,
  size_overflow,
  allocation_unavailable,
};

struct HostAllocationFailureInjection {
  std::optional<std::size_t> fail_at_allocation_ordinal;
  std::size_t allocation_ordinal{0U};
  std::optional<std::size_t> max_container_elements{};
};

enum class HostAllocationPurpose {
  array,
  buffer,
};

enum class SemanticHostAllocationPurpose : std::uint8_t {
  none,
  vector_payload,
  tuple_table,
  workspace,
};

using SemanticHostAllocationRelease =
    void (*)(std::uint64_t, std::size_t);

struct HostAllocationRecord {
  HostAllocationRecord *next;
  void *allocation;
  void *payload;
  std::size_t byte_count;
  HostAllocationPurpose purpose;
  const void *element_type;
  std::uint64_t semantic_owner_token{0U};
  std::size_t semantic_canonical_bytes{0U};
  std::size_t semantic_allocation_ordinal{0U};
  SemanticHostAllocationPurpose semantic_purpose{
      SemanticHostAllocationPurpose::none};
  SemanticHostAllocationRelease semantic_release{nullptr};
  bool semantic_active{false};
};

constexpr std::size_t host_allocation_bucket_count = 4096U;

struct HostAllocationBucket {
  HostAllocationRecord *records{nullptr};
  std::mutex mutex{};
};

inline std::array<HostAllocationBucket, host_allocation_bucket_count>
    host_allocation_buckets{};

template <typename Element>
inline const std::byte host_element_type_identity{};

inline std::size_t host_allocation_bucket_index(const void *payload) {
  const std::uintptr_t address =
      reinterpret_cast<std::uintptr_t>(payload);
  return ((address >> 4U) ^ (address >> 17U)) &
         (host_allocation_bucket_count - 1U);
}

inline void register_host_allocation(HostAllocationRecord &record) {
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(record.payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  record.next = bucket.records;
  bucket.records = &record;
}

inline HostAllocationRecord *find_host_allocation_unlocked(
    HostAllocationBucket &bucket,
    const void *payload, HostAllocationPurpose purpose,
    const void *element_type) {
  for (HostAllocationRecord *record = bucket.records;
       record != nullptr; record = record->next) {
    if (record->payload == payload && record->purpose == purpose &&
        record->element_type == element_type) {
      return record;
    }
  }
  return nullptr;
}

inline HostAllocationRecord *remove_host_allocation(
    const void *payload, HostAllocationPurpose purpose,
    const void *element_type) {
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  HostAllocationRecord **link = &bucket.records;
  while (*link != nullptr) {
    HostAllocationRecord *record = *link;
    if (record->payload == payload && record->purpose == purpose &&
        record->element_type == element_type) {
      *link = record->next;
      record->next = nullptr;
      return record;
    }
    link = &record->next;
  }
  return nullptr;
}

template <typename Element>
struct HostArrayHeader {
  HostAllocationRecord allocation_record;
  std::size_t constructed;
  std::size_t capacity;
};

template <typename Element>
void release_host_array(Element *elements) {
  if (elements == nullptr) {
    return;
  }
  HostAllocationRecord *record = remove_host_allocation(
      elements, HostAllocationPurpose::array,
      &host_element_type_identity<Element>);
  if (record == nullptr) {
    return;
  }
  auto *header = reinterpret_cast<HostArrayHeader<Element> *>(record);
  for (std::size_t index = header->constructed; index > 0U; --index) {
    std::destroy_at(elements + index - 1U);
  }
  std::free(record->allocation);
}

template <typename Element>
using HostArrayStorage =
    std::unique_ptr<Element, decltype(&release_host_array<Element>)>;

template <typename Element>
struct HostArray {
  HostArrayStorage<Element> storage{nullptr, &release_host_array<Element>};
  std::size_t size{0U};
  std::size_t capacity{0U};
};

template <typename Element>
void release_host_buffer(Element *elements) {
  if (elements == nullptr) {
    return;
  }
  HostAllocationRecord *record = remove_host_allocation(
      elements, HostAllocationPurpose::buffer,
      &host_element_type_identity<Element>);
  if (record != nullptr) {
    if (record->semantic_active &&
        record->semantic_release != nullptr) {
      record->semantic_release(record->semantic_owner_token,
                               record->semantic_canonical_bytes);
      record->semantic_active = false;
    }
    std::free(record->allocation);
  }
}

template <typename Element>
using HostBufferStorage =
    std::unique_ptr<Element, decltype(&release_host_buffer<Element>)>;

template <typename Element>
HostResourceErrorReason allocate_host_buffer(
    HostBufferStorage<Element> &storage, std::size_t element_count) {
  if (storage != nullptr) {
    return HostResourceErrorReason::size_overflow;
  }
  if (element_count == 0U) {
    return HostResourceErrorReason::none;
  }
  constexpr std::size_t header_size = sizeof(HostAllocationRecord);
  constexpr std::size_t alignment =
      alignof(Element) > alignof(HostAllocationRecord)
          ? alignof(Element)
          : alignof(HostAllocationRecord);
  constexpr std::size_t alignment_slack = alignment - 1U;
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (element_count >
      (maximum - header_size - alignment_slack) / sizeof(Element)) {
    return HostResourceErrorReason::size_overflow;
  }
  const std::size_t byte_count = element_count * sizeof(Element);
  void *allocation =
      std::malloc(header_size + alignment_slack + byte_count);
  if (allocation == nullptr) {
    return HostResourceErrorReason::allocation_unavailable;
  }
  const std::uintptr_t unaligned =
      reinterpret_cast<std::uintptr_t>(allocation) + header_size;
  const std::uintptr_t aligned =
      (unaligned + alignment_slack) &
      ~static_cast<std::uintptr_t>(alignment_slack);
  auto *elements = reinterpret_cast<Element *>(aligned);
  auto *record = new (allocation) HostAllocationRecord{
      nullptr, allocation, elements, byte_count,
      HostAllocationPurpose::buffer, &host_element_type_identity<Element>};
  register_host_allocation(*record);
  storage.reset(elements);
  return HostResourceErrorReason::none;
}

template <typename Element>
bool host_buffer_valid(const HostBufferStorage<Element> &storage,
                       std::size_t required_bytes) {
  if (storage == nullptr) {
    return required_bytes == 0U;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(storage.get())];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  const HostAllocationRecord *record = find_host_allocation_unlocked(
      bucket,
      storage.get(), HostAllocationPurpose::buffer,
      &host_element_type_identity<Element>);
  return record != nullptr && required_bytes == record->byte_count;
}

template <typename Element>
bool bind_semantic_host_buffer(
    const HostBufferStorage<Element> &storage,
    std::uint64_t owner_token, std::size_t canonical_bytes,
    std::size_t allocation_ordinal,
    SemanticHostAllocationPurpose semantic_purpose,
    SemanticHostAllocationRelease semantic_release) {
  if (storage == nullptr || owner_token == 0U ||
      semantic_purpose == SemanticHostAllocationPurpose::none ||
      semantic_release == nullptr) {
    return false;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(storage.get())];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  HostAllocationRecord *record = find_host_allocation_unlocked(
      bucket, storage.get(), HostAllocationPurpose::buffer,
      &host_element_type_identity<Element>);
  if (record == nullptr || record->semantic_active) {
    return false;
  }
  record->semantic_owner_token = owner_token;
  record->semantic_canonical_bytes = canonical_bytes;
  record->semantic_allocation_ordinal = allocation_ordinal;
  record->semantic_purpose = semantic_purpose;
  record->semantic_release = semantic_release;
  record->semantic_active = true;
  return true;
}

inline bool semantic_host_buffer_matches(
    const void *payload, std::uint64_t owner_token,
    std::size_t canonical_bytes,
    std::optional<std::size_t> allocation_ordinal,
    SemanticHostAllocationPurpose semantic_purpose) {
  if (payload == nullptr || owner_token == 0U ||
      !allocation_ordinal.has_value() ||
      semantic_purpose == SemanticHostAllocationPurpose::none) {
    return false;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(payload)];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  for (const HostAllocationRecord *record = bucket.records;
       record != nullptr; record = record->next) {
    if (record->payload == payload &&
        record->purpose == HostAllocationPurpose::buffer) {
      return record->semantic_active &&
             record->semantic_owner_token == owner_token &&
             record->semantic_canonical_bytes == canonical_bytes &&
             record->semantic_allocation_ordinal ==
                 *allocation_ordinal &&
             record->semantic_purpose == semantic_purpose;
    }
  }
  return false;
}

inline bool consume_semantic_host_buffer(
    const void *payload, std::uint64_t owner_token,
    std::size_t canonical_bytes,
    std::optional<std::size_t> allocation_ordinal,
    SemanticHostAllocationPurpose semantic_purpose) {
  if (payload == nullptr || owner_token == 0U ||
      !allocation_ordinal.has_value() ||
      semantic_purpose == SemanticHostAllocationPurpose::none) {
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
    if (!record->semantic_active ||
        record->semantic_owner_token != owner_token ||
        record->semantic_canonical_bytes != canonical_bytes ||
        record->semantic_allocation_ordinal !=
            *allocation_ordinal ||
        record->semantic_purpose != semantic_purpose) {
      return false;
    }
    record->semantic_active = false;
    return true;
  }
  return false;
}

inline HostResourceErrorReason
begin_host_allocation(HostAllocationFailureInjection &failure) {
  const std::size_t ordinal = failure.allocation_ordinal;
  if (ordinal == std::numeric_limits<std::size_t>::max()) {
    return HostResourceErrorReason::allocation_unavailable;
  }
  ++failure.allocation_ordinal;
  if (failure.fail_at_allocation_ordinal.has_value() &&
      ordinal == *failure.fail_at_allocation_ordinal) {
    return HostResourceErrorReason::allocation_unavailable;
  }
  return HostResourceErrorReason::none;
}

template <typename Element>
HostResourceErrorReason
host_array_allocation_preflight(
    const HostArray<Element> &array, std::size_t capacity,
    std::optional<std::size_t> max_container_elements) {
  if (capacity == 0U) {
    return HostResourceErrorReason::none;
  }
  if (array.storage != nullptr || array.size != 0U || array.capacity != 0U) {
    return HostResourceErrorReason::size_overflow;
  }
  if (max_container_elements.has_value() &&
      capacity > *max_container_elements) {
    return HostResourceErrorReason::size_overflow;
  }
  constexpr std::size_t header_size = sizeof(HostArrayHeader<Element>);
  constexpr std::size_t alignment =
      alignof(Element) > alignof(HostArrayHeader<Element>)
          ? alignof(Element)
          : alignof(HostArrayHeader<Element>);
  constexpr std::size_t alignment_slack = alignment - 1U;
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (capacity > (maximum - header_size - alignment_slack) / sizeof(Element)) {
    return HostResourceErrorReason::size_overflow;
  }
  return HostResourceErrorReason::none;
}

template <typename Element>
HostResourceErrorReason
allocate_host_array(HostArray<Element> &array, std::size_t capacity,
                    std::optional<std::size_t> max_container_elements) {
  const HostResourceErrorReason preflight = host_array_allocation_preflight(
      array, capacity, max_container_elements);
  if (preflight != HostResourceErrorReason::none || capacity == 0U) {
    return preflight;
  }
  constexpr std::size_t header_size = sizeof(HostArrayHeader<Element>);
  constexpr std::size_t alignment =
      alignof(Element) > alignof(HostArrayHeader<Element>)
          ? alignof(Element)
          : alignof(HostArrayHeader<Element>);
  constexpr std::size_t alignment_slack = alignment - 1U;
  const std::size_t allocation_size =
      header_size + alignment_slack + capacity * sizeof(Element);
  void *allocation = std::malloc(allocation_size);
  if (allocation == nullptr) {
    return HostResourceErrorReason::allocation_unavailable;
  }
  const std::uintptr_t unaligned =
      reinterpret_cast<std::uintptr_t>(allocation) + header_size;
  const std::uintptr_t aligned =
      (unaligned + alignment_slack) & ~static_cast<std::uintptr_t>(
                                            alignment_slack);
  auto *elements = reinterpret_cast<Element *>(aligned);
  auto *header = reinterpret_cast<HostArrayHeader<Element> *>(
      reinterpret_cast<std::byte *>(elements) - header_size);
  std::construct_at(
      header,
      HostArrayHeader<Element>{
          HostAllocationRecord{
              nullptr, allocation, elements, capacity * sizeof(Element),
              HostAllocationPurpose::array,
              &host_element_type_identity<Element>},
          0U, capacity});
  register_host_allocation(header->allocation_record);
  array.storage.reset(elements);
  array.capacity = capacity;
  return HostResourceErrorReason::none;
}

template <typename Element>
HostResourceErrorReason
allocate_host_array(HostArray<Element> &array, std::size_t capacity,
                    HostAllocationFailureInjection &failure) {
  const HostResourceErrorReason preflight = host_array_allocation_preflight(
      array, capacity, failure.max_container_elements);
  if (preflight != HostResourceErrorReason::none || capacity == 0U) {
    return preflight;
  }
  const HostResourceErrorReason begin = begin_host_allocation(failure);
  if (begin != HostResourceErrorReason::none) {
    return begin;
  }
  return allocate_host_array(array, capacity,
                             failure.max_container_elements);
}

template <typename Element, typename Source>
[[nodiscard]] HostResourceErrorReason
host_array_push(HostArray<Element> &array, Source &&source) {
  static_assert(std::is_nothrow_constructible_v<Element, Source &&>);
  if (array.storage == nullptr || array.size >= array.capacity) {
    return HostResourceErrorReason::size_overflow;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(
          array.storage.get())];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  HostAllocationRecord *record = find_host_allocation_unlocked(
      bucket,
      array.storage.get(), HostAllocationPurpose::array,
      &host_element_type_identity<Element>);
  if (record == nullptr) {
    return HostResourceErrorReason::size_overflow;
  }
  auto *header = reinterpret_cast<HostArrayHeader<Element> *>(record);
  if (array.capacity != header->capacity ||
      array.size >= header->capacity) {
    return HostResourceErrorReason::size_overflow;
  }
  std::construct_at(array.storage.get() + array.size,
                    std::forward<Source>(source));
  ++array.size;
  if (array.size > header->constructed) {
    header->constructed = array.size;
  }
  return HostResourceErrorReason::none;
}

template <typename Element>
[[nodiscard]] HostResourceErrorReason
host_array_fill(HostArray<Element> &array, std::size_t count,
                const Element &value) {
  static_assert(std::is_nothrow_copy_constructible_v<Element>);
  if (array.storage == nullptr) {
    return count == 0U && array.size == 0U && array.capacity == 0U
               ? HostResourceErrorReason::none
               : HostResourceErrorReason::size_overflow;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(
          array.storage.get())];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  HostAllocationRecord *record = find_host_allocation_unlocked(
      bucket, array.storage.get(), HostAllocationPurpose::array,
      &host_element_type_identity<Element>);
  if (record == nullptr) {
    return HostResourceErrorReason::size_overflow;
  }
  auto *header =
      reinterpret_cast<HostArrayHeader<Element> *>(record);
  if (array.capacity != header->capacity ||
      array.size > header->capacity ||
      count > header->capacity - array.size) {
    return HostResourceErrorReason::size_overflow;
  }
  for (std::size_t index = 0U; index < count; ++index) {
    std::construct_at(array.storage.get() + array.size, value);
    ++array.size;
  }
  if (array.size > header->constructed) {
    header->constructed = array.size;
  }
  return HostResourceErrorReason::none;
}

template <typename Element>
bool host_array_metadata_valid(const HostArray<Element> &array) {
  if (array.storage == nullptr) {
    return array.size == 0U && array.capacity == 0U;
  }
  HostAllocationBucket &bucket =
      host_allocation_buckets[host_allocation_bucket_index(
          array.storage.get())];
  const std::lock_guard<std::mutex> lock(bucket.mutex);
  const HostAllocationRecord *record = find_host_allocation_unlocked(
      bucket,
      array.storage.get(), HostAllocationPurpose::array,
      &host_element_type_identity<Element>);
  if (record == nullptr) {
    return false;
  }
  const auto *header =
      reinterpret_cast<const HostArrayHeader<Element> *>(record);
  return array.capacity == header->capacity &&
         array.size <= header->capacity &&
         header->constructed <= header->capacity &&
         array.size <= header->constructed;
}

template <typename Element>
bool host_array_has_capacity(const HostArray<Element> &array,
                             std::size_t count) {
  if (!host_array_metadata_valid(array)) {
    return false;
  }
  return count <= array.capacity - array.size;
}

template <typename Element>
std::span<Element> host_array_span(HostArray<Element> &array) {
  if (!host_array_metadata_valid(array)) {
    return {};
  }
  return std::span<Element>(array.storage.get(), array.size);
}

template <typename Element>
std::span<const Element> host_array_span(const HostArray<Element> &array) {
  if (!host_array_metadata_valid(array)) {
    return {};
  }
  return std::span<const Element>(array.storage.get(), array.size);
}

template <typename Element>
void reset_host_array(HostArray<Element> &array) {
  array.storage.reset();
  array.size = 0U;
  array.capacity = 0U;
}

} // namespace bennu

#endif
