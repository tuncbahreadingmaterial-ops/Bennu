#ifndef BENNU_RUNNER_ARGUMENTS_HPP
#define BENNU_RUNNER_ARGUMENTS_HPP

#include "bennu/evaluator.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bennu {

struct RunnerEvaluationResult {
  bool ok;
  std::vector<Value> values;
  std::vector<std::string> formatted;
  Error error;
};

RunnerEvaluationResult evaluate_runner_source(
    std::string_view source, std::span<const std::string_view> arguments);

} // namespace bennu

#endif
