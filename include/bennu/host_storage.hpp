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

template <typename Element>
struct HostArrayHeader {
  void *allocation;
  std::size_t constructed;
  std::size_t capacity;
};

template <typename Element>
void release_host_array(Element *elements) {
  if (elements == nullptr) {
    return;
  }
  auto *header = reinterpret_cast<HostArrayHeader<Element> *>(
      reinterpret_cast<std::byte *>(elements) -
      sizeof(HostArrayHeader<Element>));
  for (std::size_t index = header->constructed; index > 0U; --index) {
    std::destroy_at(elements + index - 1U);
  }
  std::free(header->allocation);
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
  std::construct_at(header,
                    HostArrayHeader<Element>{allocation, 0U, capacity});
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
  auto *header = reinterpret_cast<HostArrayHeader<Element> *>(
      reinterpret_cast<std::byte *>(array.storage.get()) -
      sizeof(HostArrayHeader<Element>));
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
  const auto *header = reinterpret_cast<const HostArrayHeader<Element> *>(
      reinterpret_cast<const std::byte *>(array.storage.get()) -
      sizeof(HostArrayHeader<Element>));
  if (array.capacity != header->capacity ||
      array.size > header->capacity ||
      count > header->capacity - array.size) {
    return HostResourceErrorReason::size_overflow;
  }
  for (std::size_t index = 0U; index < count; ++index) {
    const HostResourceErrorReason pushed = host_array_push(array, value);
    if (pushed != HostResourceErrorReason::none) {
      return pushed;
    }
  }
  return HostResourceErrorReason::none;
}

template <typename Element>
bool host_array_metadata_valid(const HostArray<Element> &array) {
  if (array.storage == nullptr) {
    return array.size == 0U && array.capacity == 0U;
  }
  const auto *header = reinterpret_cast<const HostArrayHeader<Element> *>(
      reinterpret_cast<const std::byte *>(array.storage.get()) -
      sizeof(HostArrayHeader<Element>));
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
  return std::span<Element>(array.storage.get(), array.size);
}

template <typename Element>
std::span<const Element> host_array_span(const HostArray<Element> &array) {
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
