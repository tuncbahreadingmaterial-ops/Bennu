#ifndef BENNU_TYPED_APPLICATION_HPP
#define BENNU_TYPED_APPLICATION_HPP

#include "bennu/application.hpp"

namespace bennu {

PrimitiveApplicationResult apply_typed_primitive(
    PrimitiveApplicationContext &context,
    const PrimitiveDescriptor &descriptor,
    PrimitiveImplementation implementation,
    std::span<const Value> arguments,
    SourceLocation call_location);

} // namespace bennu

#endif
