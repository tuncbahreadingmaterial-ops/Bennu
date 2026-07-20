#ifndef BENNU_EVALUATOR_HPP
#define BENNU_EVALUATOR_HPP

#include "bennu/error.hpp"

#include <string_view>
#include <vector>

namespace bennu {

struct ValueResult {
  bool ok;
  Value value;
  Error error;
};

struct ProgramResult {
  bool ok;
  std::vector<Value> values;
  Error error;
};

ValueResult evaluate_expression(std::string_view source);
ProgramResult evaluate_source(std::string_view source);

} // namespace bennu

#endif
