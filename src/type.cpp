#include "bennu/type.hpp"

#include "bennu/value.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace bennu {

std::span<TypeNode> type_nodes(TypeArena &type) {
  if (type.inline_leaf.has_value()) {
    return std::span<TypeNode>(&*type.inline_leaf, 1U);
  }
  return host_array_span(type.nodes);
}

std::span<const TypeNode> type_nodes(const TypeArena &type) {
  if (type.inline_leaf.has_value()) {
    return std::span<const TypeNode>(&*type.inline_leaf, 1U);
  }
  return host_array_span(type.nodes);
}

namespace {

TypeArena empty_type() {
  return TypeArena{};
}

bool valid_scalar_type(ScalarType type) {
  switch (type) {
  case ScalarType::boolean:
  case ScalarType::integer:
  case ScalarType::double_precision:
    return true;
  }
  return false;
}

TypeValidationResult valid_type() {
  return TypeValidationResult{true, TypeInvariant::none, 0U, 0U};
}

TypeValidationResult invalid_type(TypeInvariant invariant,
                                  std::size_t node_index,
                                  std::size_t edge_index) {
  return TypeValidationResult{false, invariant, node_index, edge_index};
}

TypeValidationResult type_resource_failure(HostResourceErrorReason reason) {
  return TypeValidationResult{false, TypeInvariant::none, 0U, 0U, reason};
}

TypeConstructionResult
type_construction_failure(HostResourceErrorReason reason) {
  return TypeConstructionResult{false, empty_type(), TypeInvariant::none,
                                reason};
}

TypeFormattingResult type_formatting_failure(
    TypeInvariant invariant, TypeFormatError error,
    HostResourceErrorReason resource_error) {
  TypeFormattingResult result;
  result.ok = false;
  result.invariant = invariant;
  result.error = error;
  result.resource_error = resource_error;
  return result;
}

std::string_view scalar_type_name(ScalarType type) {
  switch (type) {
  case ScalarType::boolean:
    return "Bool";
  case ScalarType::integer:
    return "Int";
  case ScalarType::double_precision:
    return "Double";
  }
  return "";
}

struct FormatAction {
  enum class Kind {
    node,
    text,
  } kind;
  std::size_t node_index;
  std::string_view text;
};

HostResourceErrorReason append_text(HostArray<char> &formatted,
                                    std::string_view text) {
  for (const char character : text) {
    const HostResourceErrorReason pushed =
        host_array_push(formatted, character);
    if (pushed != HostResourceErrorReason::none) {
      return pushed;
    }
  }
  return HostResourceErrorReason::none;
}

TypeArena make_leaf_type(TypeKind kind, ScalarType scalar) {
  TypeArena type;
  type.inline_leaf = TypeNode{kind, scalar, 0U, 0U};
  return type;
}

} // namespace

TypeArena make_scalar_type(ScalarType type) {
  return make_leaf_type(TypeKind::scalar, type);
}

TypeArena make_vector_type(ScalarType element_type) {
  return make_leaf_type(TypeKind::vector, element_type);
}

TypeConstructionResult clone_type(const TypeArena &type) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return clone_type(type, allocation_failure);
}

TypeConstructionResult clone_type(
    const TypeArena &type,
    HostAllocationFailureInjection &allocation_failure) {
  const TypeValidationResult validation =
      validate_type(type, allocation_failure);
  if (!validation.ok) {
    return TypeConstructionResult{false, empty_type(), validation.invariant,
                                  validation.resource_error};
  }
  TypeArena result;
  const std::size_t node_count = type_nodes(type).size();
  if (type.inline_leaf.has_value()) {
    result.inline_leaf = type.inline_leaf;
    return TypeConstructionResult{true, std::move(result),
                                  TypeInvariant::none};
  }
  HostResourceErrorReason allocated =
      allocate_host_array(result.nodes, node_count, allocation_failure);
  if (allocated == HostResourceErrorReason::none) {
    allocated = allocate_host_array(result.child_indexes,
                                    type.child_indexes.size,
                                    allocation_failure);
  }
  if (allocated != HostResourceErrorReason::none) {
    return type_construction_failure(allocated);
  }
  for (const TypeNode &node : type_nodes(type)) {
    const HostResourceErrorReason pushed =
        host_array_push(result.nodes, node);
    if (pushed != HostResourceErrorReason::none) {
      return type_construction_failure(pushed);
    }
  }
  for (const std::size_t child_index :
       host_array_span(type.child_indexes)) {
    const HostResourceErrorReason pushed =
        host_array_push(result.child_indexes, child_index);
    if (pushed != HostResourceErrorReason::none) {
      return type_construction_failure(pushed);
    }
  }
  result.root_index = type.root_index;
  return TypeConstructionResult{true, std::move(result),
                                TypeInvariant::none};
}

TypeConstructionResult make_tuple_type(std::span<const TypeArena> elements) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return make_tuple_type(elements, allocation_failure);
}

TypeConstructionResult make_tuple_type(
    std::span<const TypeArena> elements,
    HostAllocationFailureInjection &allocation_failure) {
  std::size_t node_count = 1U;
  std::size_t edge_count = elements.size();
  for (const TypeArena &element : elements) {
    const TypeValidationResult validation =
        validate_type(element, allocation_failure);
    if (!validation.ok) {
      return TypeConstructionResult{false, empty_type(),
                                    validation.invariant,
                                    validation.resource_error};
    }
    const std::size_t element_node_count = type_nodes(element).size();
    if (element_node_count >
            std::numeric_limits<std::size_t>::max() - node_count ||
        element.child_indexes.size >
            std::numeric_limits<std::size_t>::max() - edge_count) {
      return type_construction_failure(
          HostResourceErrorReason::size_overflow);
    }
    node_count += element_node_count;
    edge_count += element.child_indexes.size;
  }

  TypeArena result;
  HostArray<std::size_t> roots;
  HostResourceErrorReason allocated =
      allocate_host_array(result.nodes, node_count, allocation_failure);
  if (allocated == HostResourceErrorReason::none) {
    allocated = allocate_host_array(result.child_indexes, edge_count,
                                    allocation_failure);
  }
  if (allocated == HostResourceErrorReason::none) {
    allocated =
        allocate_host_array(roots, elements.size(), allocation_failure);
  }
  if (allocated != HostResourceErrorReason::none) {
    return type_construction_failure(allocated);
  }

  for (const TypeArena &element : elements) {
    const std::size_t node_offset = result.nodes.size;
    const std::size_t edge_offset = result.child_indexes.size;
    for (const TypeNode &source : type_nodes(element)) {
      TypeNode node = source;
      if (node.kind == TypeKind::tuple) {
        node.first_child += edge_offset;
      }
      const HostResourceErrorReason pushed =
          host_array_push(result.nodes, node);
      if (pushed != HostResourceErrorReason::none) {
        return type_construction_failure(pushed);
      }
    }
    for (const std::size_t child_index :
         host_array_span(element.child_indexes)) {
      const HostResourceErrorReason pushed = host_array_push(
          result.child_indexes, child_index + node_offset);
      if (pushed != HostResourceErrorReason::none) {
        return type_construction_failure(pushed);
      }
    }
    const HostResourceErrorReason root_pushed =
        host_array_push(roots, element.root_index + node_offset);
    if (root_pushed != HostResourceErrorReason::none) {
      return type_construction_failure(root_pushed);
    }
  }

  const std::size_t first_child = result.child_indexes.size;
  for (const std::size_t root : host_array_span(roots)) {
    const HostResourceErrorReason pushed =
        host_array_push(result.child_indexes, root);
    if (pushed != HostResourceErrorReason::none) {
      return type_construction_failure(pushed);
    }
  }
  const HostResourceErrorReason root_pushed = host_array_push(
      result.nodes,
      TypeNode{TypeKind::tuple, ScalarType::boolean, first_child,
               roots.size});
  if (root_pushed != HostResourceErrorReason::none) {
    return type_construction_failure(root_pushed);
  }
  result.root_index = result.nodes.size - 1U;
  return TypeConstructionResult{true, std::move(result),
                                TypeInvariant::none};
}

TypeValidationResult validate_type(const TypeArena &type) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return validate_type(type, allocation_failure);
}

TypeValidationResult validate_type(
    const TypeArena &type,
    HostAllocationFailureInjection &allocation_failure) {
  if (!host_array_metadata_valid(type.nodes) ||
      !host_array_metadata_valid(type.child_indexes) ||
      (type.inline_leaf.has_value() && type.nodes.size != 0U)) {
    return invalid_type(TypeInvariant::invalid_type_root,
                        type.root_index, 0U);
  }
  const std::span<const TypeNode> nodes = type_nodes(type);
  if (nodes.empty() || type.root_index >= nodes.size()) {
    return invalid_type(TypeInvariant::invalid_type_root,
                        type.root_index, 0U);
  }
  if (type.root_index != nodes.size() - 1U) {
    return invalid_type(TypeInvariant::nonfinal_type_root,
                        type.root_index, 0U);
  }
  if (type.child_indexes.size ==
      std::numeric_limits<std::size_t>::max()) {
    return type_resource_failure(HostResourceErrorReason::size_overflow);
  }

  HostArray<std::uint8_t> visited;
  HostArray<std::uint8_t> edge_owners;
  HostArray<std::uint8_t> parent_counts;
  HostArray<std::size_t> stack;
  HostResourceErrorReason allocated =
      allocate_host_array(visited, nodes.size(), allocation_failure);
  if (allocated == HostResourceErrorReason::none) {
    allocated = allocate_host_array(edge_owners,
                                    type.child_indexes.size,
                                    allocation_failure);
  }
  if (allocated == HostResourceErrorReason::none) {
    allocated = allocate_host_array(parent_counts, nodes.size(),
                                    allocation_failure);
  }
  if (allocated != HostResourceErrorReason::none) {
    return type_resource_failure(allocated);
  }
  allocated = host_array_fill(visited, nodes.size(), std::uint8_t{0U});
  if (allocated == HostResourceErrorReason::none) {
    allocated = host_array_fill(edge_owners, type.child_indexes.size,
                                std::uint8_t{0U});
  }
  if (allocated == HostResourceErrorReason::none) {
    allocated =
        host_array_fill(parent_counts, nodes.size(), std::uint8_t{0U});
  }
  if (allocated != HostResourceErrorReason::none) {
    return type_resource_failure(allocated);
  }
  allocated = allocate_host_array(
      stack, type.child_indexes.size + 1U, allocation_failure);
  if (allocated != HostResourceErrorReason::none) {
    return type_resource_failure(allocated);
  }

  allocated = host_array_push(stack, type.root_index);
  if (allocated != HostResourceErrorReason::none) {
    return type_resource_failure(allocated);
  }
  while (stack.size != 0U) {
    const std::size_t node_index = stack.storage.get()[stack.size - 1U];
    --stack.size;
    if (visited.storage.get()[node_index] != 0U) {
      return invalid_type(TypeInvariant::aliased_tuple_child,
                          node_index, 0U);
    }
    visited.storage.get()[node_index] = 1U;
    const TypeNode &node = nodes[node_index];
    switch (node.kind) {
    case TypeKind::scalar:
    case TypeKind::vector:
      if (!valid_scalar_type(node.scalar)) {
        return invalid_type(TypeInvariant::unknown_scalar_type,
                            node_index, 0U);
      }
      if (node.first_child != 0U || node.child_count != 0U) {
        return invalid_type(TypeInvariant::inactive_type_field,
                            node_index, 0U);
      }
      break;
    case TypeKind::tuple: {
      if (node.scalar != ScalarType::boolean) {
        return invalid_type(TypeInvariant::inactive_type_field,
                            node_index, 0U);
      }
      if (node.first_child > type.child_indexes.size ||
          node.child_count >
              type.child_indexes.size - node.first_child) {
        return invalid_type(TypeInvariant::invalid_tuple_range,
                            node_index, node.first_child);
      }
      for (std::size_t offset = 0U; offset < node.child_count; ++offset) {
        const std::size_t edge_index = node.first_child + offset;
        if (edge_owners.storage.get()[edge_index] != 0U) {
          return invalid_type(TypeInvariant::overlapping_tuple_range,
                              node_index, edge_index);
        }
        edge_owners.storage.get()[edge_index] = 1U;
        const std::size_t child_index =
            type.child_indexes.storage.get()[edge_index];
        if (child_index >= nodes.size()) {
          return invalid_type(TypeInvariant::invalid_tuple_child_index,
                              node_index, edge_index);
        }
        if (child_index >= node_index) {
          return invalid_type(TypeInvariant::non_postorder_tuple_child,
                              node_index, edge_index);
        }
        ++parent_counts.storage.get()[child_index];
        if (parent_counts.storage.get()[child_index] != 1U) {
          return invalid_type(TypeInvariant::aliased_tuple_child,
                              child_index, edge_index);
        }
      }
      for (std::size_t offset = node.child_count; offset > 0U; --offset) {
        const HostResourceErrorReason pushed = host_array_push(
            stack, type.child_indexes.storage.get()[
                       node.first_child + offset - 1U]);
        if (pushed != HostResourceErrorReason::none) {
          return type_resource_failure(pushed);
        }
      }
      break;
    }
    default:
      return invalid_type(TypeInvariant::unknown_type_kind,
                          node_index, 0U);
    }
  }

  for (std::size_t index = 0U; index < visited.size; ++index) {
    if (visited.storage.get()[index] == 0U) {
      return invalid_type(TypeInvariant::orphan_type_node, index, 0U);
    }
  }
  for (std::size_t index = 0U; index < edge_owners.size; ++index) {
    if (edge_owners.storage.get()[index] == 0U) {
      return invalid_type(TypeInvariant::orphan_tuple_edge, 0U, index);
    }
  }
  return valid_type();
}

bool structural_type_equal(const TypeArena &left, const TypeArena &right) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  const TypeEqualityResult result =
      structural_type_equal(left, right, allocation_failure);
  return result.ok && result.equal;
}

TypeEqualityResult structural_type_equal(
    const TypeArena &left, const TypeArena &right,
    HostAllocationFailureInjection &allocation_failure) {
  const TypeValidationResult left_validation =
      validate_type(left, allocation_failure);
  if (!left_validation.ok) {
    return TypeEqualityResult{false, false, left_validation.invariant,
                              left_validation.resource_error};
  }
  const TypeValidationResult right_validation =
      validate_type(right, allocation_failure);
  if (!right_validation.ok) {
    return TypeEqualityResult{false, false, right_validation.invariant,
                              right_validation.resource_error};
  }

  using NodePair = std::pair<std::size_t, std::size_t>;
  const std::span<const TypeNode> left_nodes = type_nodes(left);
  const std::span<const TypeNode> right_nodes = type_nodes(right);
  HostArray<NodePair> stack;
  const HostResourceErrorReason allocated = allocate_host_array(
      stack, std::min(left_nodes.size(), right_nodes.size()),
      allocation_failure);
  if (allocated != HostResourceErrorReason::none) {
    return TypeEqualityResult{false, false, TypeInvariant::none,
                              allocated};
  }
  const HostResourceErrorReason root_pushed =
      host_array_push(stack, NodePair{left.root_index, right.root_index});
  if (root_pushed != HostResourceErrorReason::none) {
    return TypeEqualityResult{false, false, TypeInvariant::none,
                              root_pushed};
  }
  while (stack.size != 0U) {
    const NodePair indexes = stack.storage.get()[stack.size - 1U];
    --stack.size;
    const TypeNode &left_node = left_nodes[indexes.first];
    const TypeNode &right_node = right_nodes[indexes.second];
    if (left_node.kind != right_node.kind) {
      return TypeEqualityResult{true, false, TypeInvariant::none};
    }
    if ((left_node.kind == TypeKind::scalar ||
         left_node.kind == TypeKind::vector) &&
        left_node.scalar != right_node.scalar) {
      return TypeEqualityResult{true, false, TypeInvariant::none};
    }
    if (left_node.kind != TypeKind::tuple) {
      continue;
    }
    if (left_node.child_count != right_node.child_count) {
      return TypeEqualityResult{true, false, TypeInvariant::none};
    }
    for (std::size_t offset = 0U; offset < left_node.child_count;
         ++offset) {
      const HostResourceErrorReason pushed = host_array_push(
          stack,
          NodePair{
              left.child_indexes.storage.get()[left_node.first_child +
                                               offset],
              right.child_indexes.storage.get()[right_node.first_child +
                                                offset]});
      if (pushed != HostResourceErrorReason::none) {
        return TypeEqualityResult{false, false, TypeInvariant::none,
                                  pushed};
      }
    }
  }
  return TypeEqualityResult{true, true, TypeInvariant::none};
}

TypeFormattingResult format_type(const TypeArena &type) {
  HostAllocationFailureInjection allocation_failure{std::nullopt, 0U};
  return format_type(type, allocation_failure);
}

TypeFormattingResult format_type(
    const TypeArena &type,
    HostAllocationFailureInjection &allocation_failure) {
  const TypeValidationResult validation =
      validate_type(type, allocation_failure);
  if (!validation.ok) {
    if (validation.resource_error != HostResourceErrorReason::none) {
      return type_formatting_failure(
          TypeInvariant::none, TypeFormatError::none,
          validation.resource_error);
    }
    return type_formatting_failure(
        validation.invariant, TypeFormatError::invalid_type,
        HostResourceErrorReason::none);
  }

  const std::span<const TypeNode> nodes = type_nodes(type);
  if (nodes.size() >
          std::numeric_limits<std::size_t>::max() / 16U ||
      type.child_indexes.size >
          (std::numeric_limits<std::size_t>::max() -
           nodes.size() * 16U) /
              2U) {
    return type_formatting_failure(
        TypeInvariant::none, TypeFormatError::none,
        HostResourceErrorReason::size_overflow);
  }
  const std::size_t text_capacity =
      nodes.size() * 16U + type.child_indexes.size * 2U;
  if (type.child_indexes.size >
      (std::numeric_limits<std::size_t>::max() - nodes.size() - 1U) /
          2U) {
    return type_formatting_failure(
        TypeInvariant::none, TypeFormatError::none,
        HostResourceErrorReason::size_overflow);
  }
  const std::size_t stack_capacity =
      nodes.size() + type.child_indexes.size * 2U + 1U;

  TypeFormattingResult result;
  result.ok = false;
  result.invariant = TypeInvariant::none;
  result.error = TypeFormatError::none;
  HostArray<FormatAction> stack;
  HostResourceErrorReason allocated = allocate_host_array(
      result.formatted_storage, text_capacity, allocation_failure);
  if (allocated == HostResourceErrorReason::none) {
    allocated = allocate_host_array(stack, stack_capacity,
                                    allocation_failure);
  }
  if (allocated != HostResourceErrorReason::none) {
    result.resource_error = allocated;
    return result;
  }

  allocated = host_array_push(
      stack, FormatAction{FormatAction::Kind::node,
                          type.root_index, {}});
  if (allocated != HostResourceErrorReason::none) {
    result.resource_error = allocated;
    return result;
  }
  while (stack.size != 0U) {
    const FormatAction action =
        stack.storage.get()[stack.size - 1U];
    --stack.size;
    if (action.kind == FormatAction::Kind::text) {
      allocated = append_text(result.formatted_storage, action.text);
      if (allocated != HostResourceErrorReason::none) {
        result.resource_error = allocated;
        return result;
      }
      continue;
    }
    const TypeNode &node = nodes[action.node_index];
    if (node.kind == TypeKind::scalar) {
      allocated = append_text(result.formatted_storage,
                              scalar_type_name(node.scalar));
      if (allocated != HostResourceErrorReason::none) {
        result.resource_error = allocated;
        return result;
      }
      continue;
    }
    if (node.kind == TypeKind::vector) {
      allocated = append_text(result.formatted_storage, "Vector<");
      if (allocated == HostResourceErrorReason::none) {
        allocated = append_text(result.formatted_storage,
                                scalar_type_name(node.scalar));
      }
      if (allocated == HostResourceErrorReason::none) {
        allocated = append_text(result.formatted_storage, ">");
      }
      if (allocated != HostResourceErrorReason::none) {
        result.resource_error = allocated;
        return result;
      }
      continue;
    }

    allocated = append_text(result.formatted_storage, "Tuple<");
    if (allocated != HostResourceErrorReason::none) {
      result.resource_error = allocated;
      return result;
    }
    allocated = host_array_push(
        stack, FormatAction{FormatAction::Kind::text, 0U, ">"});
    if (allocated != HostResourceErrorReason::none) {
      result.resource_error = allocated;
      return result;
    }
    for (std::size_t offset = node.child_count; offset > 0U; --offset) {
      const std::size_t child_offset = offset - 1U;
      allocated = host_array_push(
          stack,
          FormatAction{
              FormatAction::Kind::node,
              type.child_indexes.storage.get()[
                  node.first_child + child_offset],
              {}});
      if (allocated != HostResourceErrorReason::none) {
        result.resource_error = allocated;
        return result;
      }
      if (child_offset != 0U) {
        allocated = host_array_push(
            stack,
            FormatAction{FormatAction::Kind::text, 0U, ", "});
        if (allocated != HostResourceErrorReason::none) {
          result.resource_error = allocated;
          return result;
        }
      }
    }
  }
  result.ok = true;
  result.formatted = std::string_view(
      result.formatted_storage.storage.get(),
      result.formatted_storage.size);
  return result;
}

} // namespace bennu
