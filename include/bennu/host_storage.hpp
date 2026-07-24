#ifndef BENNU_HOST_STORAGE_HPP
#define BENNU_HOST_STORAGE_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
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

struct HostAllocationSnapshot {
  bool found;
  void *allocation;
  std::size_t byte_count;
  std::size_t constructed;
  std::size_t capacity;
};

namespace detail {

std::size_t host_allocation_header_size();
std::size_t host_allocation_header_alignment();
bool register_host_allocation(
    void *allocation, void *payload, std::size_t byte_count,
    HostAllocationPurpose purpose, const void *element_type,
    std::size_t constructed, std::size_t capacity);
HostAllocationSnapshot find_host_allocation(
    const void *payload, HostAllocationPurpose purpose,
    const void *element_type);
HostAllocationSnapshot remove_host_allocation(
    const void *payload, HostAllocationPurpose purpose,
    const void *element_type);
bool claim_host_array_elements(
    const void *payload, const void *element_type,
    std::size_t expected_capacity, std::size_t current_size,
    std::size_t count);

} // namespace detail

template <typename Element>
inline const std::byte host_element_type_identity{};

template <typename Element>
void release_host_array(Element *elements) {
  if (elements == nullptr) {
    return;
  }
  const HostAllocationSnapshot record = detail::remove_host_allocation(
      elements, HostAllocationPurpose::array,
      &host_element_type_identity<Element>);
  if (!record.found) {
    return;
  }
  for (std::size_t index = record.constructed; index > 0U; --index) {
    std::destroy_at(elements + index - 1U);
  }
  std::free(record.allocation);
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
  const HostAllocationSnapshot record = detail::remove_host_allocation(
      elements, HostAllocationPurpose::buffer,
      &host_element_type_identity<Element>);
  if (record.found) {
    std::free(record.allocation);
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
  const std::size_t header_size = detail::host_allocation_header_size();
  const std::size_t header_alignment =
      detail::host_allocation_header_alignment();
  const std::size_t alignment =
      alignof(Element) > header_alignment
          ? alignof(Element)
          : header_alignment;
  const std::size_t alignment_slack = alignment - 1U;
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
  if (!detail::register_host_allocation(
          allocation, elements, byte_count,
          HostAllocationPurpose::buffer,
          &host_element_type_identity<Element>, 0U, 0U)) {
    std::free(allocation);
    return HostResourceErrorReason::allocation_unavailable;
  }
  storage.reset(elements);
  return HostResourceErrorReason::none;
}

template <typename Element>
bool host_buffer_valid(const HostBufferStorage<Element> &storage,
                       std::size_t required_bytes) {
  if (storage == nullptr) {
    return required_bytes == 0U;
  }
  const HostAllocationSnapshot record = detail::find_host_allocation(
      storage.get(), HostAllocationPurpose::buffer,
      &host_element_type_identity<Element>);
  return record.found && required_bytes == record.byte_count;
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
  const std::size_t header_size = detail::host_allocation_header_size();
  const std::size_t header_alignment =
      detail::host_allocation_header_alignment();
  const std::size_t alignment =
      alignof(Element) > header_alignment
          ? alignof(Element)
          : header_alignment;
  const std::size_t alignment_slack = alignment - 1U;
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
  const std::size_t header_size = detail::host_allocation_header_size();
  const std::size_t header_alignment =
      detail::host_allocation_header_alignment();
  const std::size_t alignment =
      alignof(Element) > header_alignment
          ? alignof(Element)
          : header_alignment;
  const std::size_t alignment_slack = alignment - 1U;
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
  if (!detail::register_host_allocation(
          allocation, elements, capacity * sizeof(Element),
          HostAllocationPurpose::array,
          &host_element_type_identity<Element>, 0U, capacity)) {
    std::free(allocation);
    return HostResourceErrorReason::allocation_unavailable;
  }
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
  if (!detail::claim_host_array_elements(
          array.storage.get(), &host_element_type_identity<Element>,
          array.capacity, array.size, 1U)) {
    return HostResourceErrorReason::size_overflow;
  }
  std::construct_at(array.storage.get() + array.size,
                    std::forward<Source>(source));
  ++array.size;
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
  if (!detail::claim_host_array_elements(
          array.storage.get(), &host_element_type_identity<Element>,
          array.capacity, array.size, count)) {
    return HostResourceErrorReason::size_overflow;
  }
  for (std::size_t index = 0U; index < count; ++index) {
    std::construct_at(array.storage.get() + array.size, value);
    ++array.size;
  }
  return HostResourceErrorReason::none;
}

template <typename Element>
bool host_array_metadata_valid(const HostArray<Element> &array) {
  if (array.storage == nullptr) {
    return array.size == 0U && array.capacity == 0U;
  }
  const HostAllocationSnapshot record = detail::find_host_allocation(
      array.storage.get(), HostAllocationPurpose::array,
      &host_element_type_identity<Element>);
  if (!record.found) {
    return false;
  }
  return array.capacity == record.capacity &&
         array.size <= record.capacity &&
         record.constructed <= record.capacity &&
         array.size <= record.constructed;
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
