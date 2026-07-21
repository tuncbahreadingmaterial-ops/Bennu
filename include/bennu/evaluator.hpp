#ifndef BENNU_EVALUATOR_HPP
#define BENNU_EVALUATOR_HPP

#include "bennu/resources.hpp"

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

struct EvaluationConfiguration {
  ExecutionProfile profile;
  ResourceLimits limits;
  AllocationFailureInjection allocation_failure{};
};

ValueResult evaluate_expression(std::string_view source);
ValueResult evaluate_expression(std::string_view source,
                                const EvaluationConfiguration &configuration);
ProgramResult evaluate_source(std::string_view source);
ProgramResult evaluate_source(std::string_view source,
                              const EvaluationConfiguration &configuration);

} // namespace bennu

#endif
