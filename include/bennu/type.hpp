#ifndef BENNU_TYPE_HPP
#define BENNU_TYPE_HPP

#include <cstddef>
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

struct TypeValidationResult {
  bool ok;
  TypeInvariant invariant;
  std::size_t node_index;
  std::size_t edge_index;
};

struct TypeConstructionResult {
  bool ok;
  TypeArena type;
  TypeInvariant invariant;
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
};

TypeArena make_scalar_type(ScalarType type);
TypeArena make_vector_type(ScalarType element_type);
TypeConstructionResult make_tuple_type(std::span<const TypeArena> elements);
TypeValidationResult validate_type(const TypeArena &type);
bool structural_type_equal(const TypeArena &left, const TypeArena &right);
TypeFormattingResult format_type(const TypeArena &type);

} // namespace bennu

#endif
