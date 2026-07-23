#ifndef BENNU_TYPE_HPP
#define BENNU_TYPE_HPP

#include "bennu/host_storage.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace bennu {

enum class ScalarType;

enum class TypeKind {
  scalar,
  vector,
  tuple,
};

struct TypeNode {
  TypeKind kind;
  ScalarType scalar;
  std::size_t first_child;
  std::size_t child_count;
};

struct TypeArena {
  HostArray<TypeNode> nodes{};
  HostArray<std::size_t> child_indexes{};
  std::size_t root_index{0U};
  std::optional<TypeNode> inline_leaf{};
};

std::span<TypeNode> type_nodes(TypeArena &type);
std::span<const TypeNode> type_nodes(const TypeArena &type);

enum class TypeInvariant {
  none,
  invalid_type_root,
  nonfinal_type_root,
  unknown_type_kind,
  unknown_scalar_type,
  inactive_type_field,
  invalid_tuple_range,
  overlapping_tuple_range,
  invalid_tuple_child_index,
  non_postorder_tuple_child,
  aliased_tuple_child,
  orphan_type_node,
  orphan_tuple_edge,
};

struct TypeValidationResult {
  bool ok;
  TypeInvariant invariant;
  std::size_t node_index;
  std::size_t edge_index;
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

struct TypeConstructionResult {
  bool ok;
  TypeArena type;
  TypeInvariant invariant;
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

struct TypeEqualityResult {
  bool ok;
  bool equal;
  TypeInvariant invariant;
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

enum class TypeFormatError {
  none,
  invalid_type,
  conversion_failure,
};

struct TypeFormattingResult {
  bool ok;
  HostArray<char> formatted_storage{};
  std::string_view formatted{};
  TypeInvariant invariant;
  TypeFormatError error;
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

TypeArena make_scalar_type(ScalarType type);
TypeArena make_vector_type(ScalarType element_type);
TypeConstructionResult clone_type(const TypeArena &type);
TypeConstructionResult clone_type(
    const TypeArena &type,
    HostAllocationFailureInjection &allocation_failure);
TypeConstructionResult make_tuple_type(std::span<const TypeArena> elements);
TypeConstructionResult make_tuple_type(
    std::span<const TypeArena> elements,
    HostAllocationFailureInjection &allocation_failure);
TypeValidationResult validate_type(const TypeArena &type);
TypeValidationResult validate_type(
    const TypeArena &type,
    HostAllocationFailureInjection &allocation_failure);
bool structural_type_equal(const TypeArena &left, const TypeArena &right);
TypeEqualityResult structural_type_equal(
    const TypeArena &left, const TypeArena &right,
    HostAllocationFailureInjection &allocation_failure);
TypeFormattingResult format_type(const TypeArena &type);
TypeFormattingResult format_type(
    const TypeArena &type,
    HostAllocationFailureInjection &allocation_failure);

} // namespace bennu

#endif
