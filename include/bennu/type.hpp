#ifndef BENNU_TYPE_HPP
#define BENNU_TYPE_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
  std::vector<TypeNode> nodes;
  std::vector<std::size_t> child_indexes;
  std::size_t root_index;
};

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

enum class HostResourceErrorReason {
  none,
  size_overflow,
  allocation_unavailable,
};

struct HostAllocationFailureInjection {
  std::optional<std::size_t> fail_at_allocation_ordinal;
  std::size_t allocation_ordinal{0U};
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
  std::string formatted;
  TypeInvariant invariant;
  TypeFormatError error;
  HostResourceErrorReason resource_error{HostResourceErrorReason::none};
};

TypeArena make_scalar_type(ScalarType type);
TypeArena make_vector_type(ScalarType element_type);
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
