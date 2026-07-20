#ifndef BENNU_APPLICATION_HPP
#define BENNU_APPLICATION_HPP

#include "bennu/error.hpp"
#include "bennu/primitive.hpp"
#include "bennu/resources.hpp"
#include "bennu/value.hpp"

#include <cstddef>
#include <span>

namespace bennu {

struct PrimitiveApplicationContext {
  EvaluationResources &resources;
  std::size_t scalar_kernel_invocations;
};

struct PrimitiveApplicationResult {
  bool ok;
  Value value;
  Error error;
};

PrimitiveApplicationResult
apply_primitive(PrimitiveApplicationContext &context,
                const PrimitiveDescriptor &descriptor,
                std::span<const Value> arguments,
                SourceLocation call_location);

} // namespace bennu

#endif
