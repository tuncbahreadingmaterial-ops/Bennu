#ifndef BENNU_C_EMITTER_HPP
#define BENNU_C_EMITTER_HPP

#include "bennu/evaluator.hpp"

#include <string>
#include <string_view>

namespace bennu {

struct CEmissionResult {
  bool ok;
  std::string source;
  Error error;
};

CEmissionResult emit_c_source(std::string_view source);

} // namespace bennu

#endif
