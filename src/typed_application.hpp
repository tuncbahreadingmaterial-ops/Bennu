#ifndef BENNU_TYPED_APPLICATION_HPP
#define BENNU_TYPED_APPLICATION_HPP

#include "bennu/application.hpp"

namespace bennu {

struct TypedPrimitiveArgument {
  const Value *owner;
  std::optional<std::size_t> tuple_node_index;
};

PrimitiveApplicationResult apply_typed_primitive(
    PrimitiveApplicationContext &context,
    const PrimitiveDescriptor &descriptor,
    PrimitiveImplementation implementation,
    std::span<const TypedPrimitiveArgument> arguments,
    SourceLocation call_location);

PrimitiveApplicationResult apply_typed_primitive(
    PrimitiveApplicationContext &context,
    const PrimitiveDescriptor &descriptor,
    PrimitiveImplementation implementation,
    std::span<const Value *const> arguments,
    SourceLocation call_location);

} // namespace bennu

#endif
