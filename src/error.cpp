#include "bennu/error.hpp"

#include "doctest/doctest.h"

#include <ostream>
#include <utility>

namespace bennu {

Error make_error(ErrorKind kind, SourceLocation location, std::string message) {
  return Error{kind, location, std::move(message)};
}

Error make_error(ErrorKind kind, SourceLocation location) {
  return Error{kind, location, {}};
}

std::optional<PrimitiveErrorContext> make_primitive_error_context(
    std::string_view name, std::optional<PrimitiveId> id) {
  PrimitiveErrorContext context;
  const HostResourceErrorReason allocated =
      allocate_host_array(context.name_storage, name.size(), std::nullopt);
  if (allocated != HostResourceErrorReason::none) {
    return std::nullopt;
  }
  for (const char character : name) {
    if (host_array_push(context.name_storage, character) !=
        HostResourceErrorReason::none) {
      return std::nullopt;
    }
  }
  context.name = std::string_view(context.name_storage.storage.get(),
                                  context.name_storage.size);
  context.id = id;
  return context;
}

TEST_CASE("errors retain semantic context independently of presentation text") {
  std::string dynamic_name = "owned-primitive";
  Error owned_name =
      make_error(ErrorKind::unknown_name, SourceLocation{0, 1, 1});
  owned_name.primitive =
      make_primitive_error_context(dynamic_name, std::nullopt);
  dynamic_name.assign(dynamic_name.size(), 'x');
  Error moved_name = std::move(owned_name);
  REQUIRE(moved_name.primitive.has_value());
  CHECK(moved_name.primitive->name == "owned-primitive");

  Error shape =
      make_error(ErrorKind::shape_mismatch, SourceLocation{8, 2, 3});
  shape.primitive =
      make_primitive_error_context("add", std::nullopt);
  shape.argument_position = 2;
  shape.shape = ShapeErrorContext{{2}, {3}};

  REQUIRE(shape.primitive.has_value());
  CHECK(shape.primitive->name == "add");
  REQUIRE(shape.argument_position.has_value());
  CHECK(*shape.argument_position == 2);
  REQUIRE(shape.shape.has_value());
  CHECK(shape.shape->expected == std::vector<std::size_t>{2});
  CHECK(shape.shape->actual == std::vector<std::size_t>{3});
  CHECK(shape.message.empty());
  CHECK_FALSE(shape.resource.has_value());
  CHECK_FALSE(shape.domain.has_value());

  Error resource =
      make_error(ErrorKind::resource_error, SourceLocation{0, 1, 1});
  resource.primitive =
      make_primitive_error_context("iota", std::nullopt);
  resource.resource = ResourceErrorContext{
      ResourceErrorReason::profile_limit,
      4,
      32,
      "bounded-v1",
      ResourceLimitKind::max_vector_bytes,
      24,
      0,
      32,
  };
  REQUIRE(resource.resource.has_value());
  CHECK(resource.resource->reason == ResourceErrorReason::profile_limit);
  CHECK(resource.resource->profile == "bounded-v1");
  REQUIRE(resource.resource->limit_kind.has_value());
  CHECK(*resource.resource->limit_kind == ResourceLimitKind::max_vector_bytes);

  Error domain = make_error(ErrorKind::domain_error, SourceLocation{0, 1, 1});
  domain.primitive =
      make_primitive_error_context("inc", std::nullopt);
  domain.element_index = 7;
  domain.domain = DomainErrorContext{
      DomainErrorReason::integer_overflow,
      ScalarSignatureContext{{ScalarType::integer}, ScalarType::integer},
      {ScalarValue{ScalarType::integer, false, INT64_MAX, 0.0}},
  };
  REQUIRE(domain.domain.has_value());
  CHECK(domain.domain->reason == DomainErrorReason::integer_overflow);
  REQUIRE(domain.element_index.has_value());
  CHECK(*domain.element_index == 7);
  REQUIRE(domain.domain->operands.size() == 1);
  CHECK(domain.domain->operands[0].integer == INT64_MAX);
}

} // namespace bennu
