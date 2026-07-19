#include "bennu/native_builder.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void test_selection_precedence() {
  const bennu::CompilerSelection explicit_selection =
      bennu::select_c_compiler("explicit cc", "environment cc",
                               bennu::NativePlatform::gcc_like);
  check(explicit_selection.ok && explicit_selection.executable == "explicit cc",
        "explicit compiler wins over CC");
  check(explicit_selection.configuration ==
            bennu::CompilerConfiguration::explicit_option,
        "explicit selection records its configuration source");

  const bennu::CompilerSelection environment_selection =
      bennu::select_c_compiler("", "environment cc",
                               bennu::NativePlatform::gcc_like);
  check(environment_selection.ok &&
            environment_selection.executable == "environment cc",
        "nonempty CC wins over fallback");
  check(environment_selection.configuration ==
            bennu::CompilerConfiguration::environment,
        "environment selection records its configuration source");
}

void test_platform_command_lines_preserve_argument_boundaries() {
  const std::vector<std::string> gcc = bennu::make_c_compiler_arguments(
      bennu::NativePlatform::gcc_like, "clang", "/tmp/source ; $.c",
      "/tmp/output ; $");
  check(gcc == std::vector<std::string>{"-std=c11", "/tmp/source ; $.c", "-o",
                                         "/tmp/output ; $"},
        "GCC-style command uses separate inert arguments");

  const std::vector<std::string> msvc = bennu::make_c_compiler_arguments(
      bennu::NativePlatform::windows_msvc, "cl.exe", "C:\\source ; $.c",
      "C:\\output ; $.exe");
  check(msvc == std::vector<std::string>{"/nologo", "/std:c11",
                                          "C:\\source ; $.c",
                                          "/Fe:C:\\output ; $.exe"},
        "MSVC command uses target-native driver arguments");
}
} // namespace

int main() {
  test_selection_precedence();
  test_platform_command_lines_preserve_argument_boundaries();
  return failures == 0 ? 0 : 1;
}
