// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN is expanded here so central
// resource teardown can run before the test process returns.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

namespace bennu::detail {
bool release_all_evaluation_resources_for_testing();
}

int main(int argument_count, char **arguments) {
  doctest::Context context(argument_count, arguments);
  const int test_result = context.run();
  const bool resources_released =
      bennu::detail::release_all_evaluation_resources_for_testing();
  return test_result == 0 && resources_released ? 0 : 1;
}
