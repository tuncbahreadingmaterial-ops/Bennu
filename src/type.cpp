#include "bennu/type.hpp"

#include "bennu/value.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace bennu {

namespace {

TypeArena empty_type() {
  return TypeArena{{}, {}, 0U};
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

} // namespace

TypeArena make_scalar_type(ScalarType type) {
  return TypeArena{{TypeNode{TypeKind::scalar, type, 0U, 0U}}, {}, 0U};
}

TypeArena make_vector_type(ScalarType element_type) {
  return TypeArena{{TypeNode{TypeKind::vector, element_type, 0U, 0U}}, {}, 0U};
}

TypeConstructionResult make_tuple_type(std::span<const TypeArena> elements) {
  std::size_t node_count = 1U;
  std::size_t edge_count = elements.size();
  for (const TypeArena &element : elements) {
    const TypeValidationResult validation = validate_type(element);
    if (!validation.ok) {
      return TypeConstructionResult{false, empty_type(), validation.invariant};
    }
    if (element.nodes.size() >
            std::numeric_limits<std::size_t>::max() - node_count ||
        element.child_indexes.size() >
            std::numeric_limits<std::size_t>::max() - edge_count) {
      return TypeConstructionResult{false, empty_type(),
                                    TypeInvariant::invalid_tuple_range};
    }
    node_count += element.nodes.size();
    edge_count += element.child_indexes.size();
  }

  TypeArena result;
  result.nodes.reserve(node_count);
  result.child_indexes.reserve(edge_count);
  std::vector<std::size_t> roots;
  roots.reserve(elements.size());
  for (const TypeArena &element : elements) {
    const std::size_t node_offset = result.nodes.size();
    const std::size_t edge_offset = result.child_indexes.size();
    for (const TypeNode &source : element.nodes) {
      TypeNode node = source;
      if (node.kind == TypeKind::tuple) {
        node.first_child += edge_offset;
      }
      result.nodes.push_back(node);
    }
    for (const std::size_t child_index : element.child_indexes) {
      result.child_indexes.push_back(child_index + node_offset);
    }
    roots.push_back(element.root_index + node_offset);
  }

  const std::size_t first_child = result.child_indexes.size();
  result.child_indexes.insert(result.child_indexes.end(), roots.begin(),
                              roots.end());
  result.nodes.push_back(TypeNode{TypeKind::tuple, ScalarType::boolean,
                                  first_child, roots.size()});
  result.root_index = result.nodes.size() - 1U;
  return TypeConstructionResult{true, std::move(result), TypeInvariant::none};
}

TypeValidationResult validate_type(const TypeArena &type) {
  if (type.nodes.empty() || type.root_index >= type.nodes.size()) {
    return invalid_type(TypeInvariant::invalid_type_root, type.root_index, 0U);
  }
  if (type.root_index != type.nodes.size() - 1U) {
    return invalid_type(TypeInvariant::nonfinal_type_root, type.root_index, 0U);
  }

  std::vector<std::uint8_t> visited(type.nodes.size(), 0U);
  std::vector<std::uint8_t> edge_owners(type.child_indexes.size(), 0U);
  std::vector<std::size_t> parent_count(type.nodes.size(), 0U);
  std::vector<std::size_t> stack{type.root_index};
  while (!stack.empty()) {
    const std::size_t node_index = stack.back();
    stack.pop_back();
    if (visited[node_index] != 0U) {
      return invalid_type(TypeInvariant::aliased_tuple_child, node_index, 0U);
    }
    visited[node_index] = 1U;
    const TypeNode &node = type.nodes[node_index];
    switch (node.kind) {
    case TypeKind::scalar:
    case TypeKind::vector:
      if (!valid_scalar_type(node.scalar)) {
        return invalid_type(TypeInvariant::unknown_scalar_type, node_index, 0U);
      }
      if (node.first_child != 0U || node.child_count != 0U) {
        return invalid_type(TypeInvariant::inactive_type_field, node_index, 0U);
      }
      break;
    case TypeKind::tuple: {
      if (node.scalar != ScalarType::boolean) {
        return invalid_type(TypeInvariant::inactive_type_field, node_index, 0U);
      }
      if (node.first_child > type.child_indexes.size() ||
          node.child_count >
              type.child_indexes.size() - node.first_child) {
        return invalid_type(TypeInvariant::invalid_tuple_range, node_index,
                            node.first_child);
      }
      for (std::size_t offset = 0U; offset < node.child_count; ++offset) {
        const std::size_t edge_index = node.first_child + offset;
        if (edge_owners[edge_index] != 0U) {
          return invalid_type(TypeInvariant::overlapping_tuple_range,
                              node_index, edge_index);
        }
        edge_owners[edge_index] = 1U;
        const std::size_t child_index = type.child_indexes[edge_index];
        if (child_index >= type.nodes.size()) {
          return invalid_type(TypeInvariant::invalid_tuple_child_index,
                              node_index, edge_index);
        }
        if (child_index >= node_index) {
          return invalid_type(TypeInvariant::non_postorder_tuple_child,
                              node_index, edge_index);
        }
        ++parent_count[child_index];
        if (parent_count[child_index] != 1U) {
          return invalid_type(TypeInvariant::aliased_tuple_child, child_index,
                              edge_index);
        }
      }
      for (std::size_t offset = node.child_count; offset > 0U; --offset) {
        stack.push_back(type.child_indexes[node.first_child + offset - 1U]);
      }
      break;
    }
    default:
      return invalid_type(TypeInvariant::unknown_type_kind, node_index, 0U);
    }
  }

  for (std::size_t index = 0U; index < visited.size(); ++index) {
    if (visited[index] == 0U) {
      return invalid_type(TypeInvariant::orphan_type_node, index, 0U);
    }
  }
  for (std::size_t index = 0U; index < edge_owners.size(); ++index) {
    if (edge_owners[index] == 0U) {
      return invalid_type(TypeInvariant::orphan_tuple_edge, 0U, index);
    }
  }
  return valid_type();
}

bool structural_type_equal(const TypeArena &left, const TypeArena &right) {
  if (!validate_type(left).ok || !validate_type(right).ok) {
    return false;
  }
  std::vector<std::pair<std::size_t, std::size_t>> stack;
  stack.emplace_back(left.root_index, right.root_index);
  while (!stack.empty()) {
    const auto [left_index, right_index] = stack.back();
    stack.pop_back();
    const TypeNode &left_node = left.nodes[left_index];
    const TypeNode &right_node = right.nodes[right_index];
    if (left_node.kind != right_node.kind) {
      return false;
    }
    if ((left_node.kind == TypeKind::scalar ||
         left_node.kind == TypeKind::vector) &&
        left_node.scalar != right_node.scalar) {
      return false;
    }
    if (left_node.kind != TypeKind::tuple) {
      continue;
    }
    if (left_node.child_count != right_node.child_count) {
      return false;
    }
    for (std::size_t offset = 0U; offset < left_node.child_count; ++offset) {
      stack.emplace_back(
          left.child_indexes[left_node.first_child + offset],
          right.child_indexes[right_node.first_child + offset]);
    }
  }
  return true;
}

TypeFormattingResult format_type(const TypeArena &type) {
  const TypeValidationResult validation = validate_type(type);
  if (!validation.ok) {
    return TypeFormattingResult{false, {}, validation.invariant,
                                TypeFormatError::invalid_type};
  }

  std::string formatted;
  std::vector<FormatAction> stack;
  stack.push_back(FormatAction{FormatAction::Kind::node, type.root_index, {}});
  while (!stack.empty()) {
    const FormatAction action = stack.back();
    stack.pop_back();
    if (action.kind == FormatAction::Kind::text) {
      formatted += action.text;
      continue;
    }
    const TypeNode &node = type.nodes[action.node_index];
    if (node.kind == TypeKind::scalar) {
      formatted += scalar_type_name(node.scalar);
      continue;
    }
    if (node.kind == TypeKind::vector) {
      formatted += "Vector<";
      formatted += scalar_type_name(node.scalar);
      formatted += '>';
      continue;
    }

    formatted += "Tuple<";
    stack.push_back(FormatAction{FormatAction::Kind::text, 0U, ">"});
    for (std::size_t offset = node.child_count; offset > 0U; --offset) {
      const std::size_t child_offset = offset - 1U;
      stack.push_back(FormatAction{
          FormatAction::Kind::node,
          type.child_indexes[node.first_child + child_offset], {}});
      if (child_offset != 0U) {
        stack.push_back(
            FormatAction{FormatAction::Kind::text, 0U, ", "});
      }
    }
  }
  return TypeFormattingResult{true, std::move(formatted), TypeInvariant::none,
                              TypeFormatError::none};
}

} // namespace bennu
