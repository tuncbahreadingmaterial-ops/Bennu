#include "bennu/c_emitter.hpp"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void test_emission_is_deterministic_and_specializes_used_value_kinds() {
  const bennu::CEmissionResult scalar = bennu::emit_c_source("inc 5");
  const bennu::CEmissionResult scalar_repeat = bennu::emit_c_source("inc 5");
  const bennu::CEmissionResult array = bennu::emit_c_source("ioata 0");

  check(scalar.ok && scalar.source == scalar_repeat.source,
        "identical scalar source emits identical C bytes");
  check(scalar.source.find("bennu_print_integer") != std::string::npos,
        "scalar emission includes integer formatting");
  check(scalar.source.find("bennu_print_array") == std::string::npos,
        "scalar emission omits unused array formatting");
  check(array.ok && array.source.find("bennu_print_array") != std::string::npos,
        "array emission includes array formatting");
  check(array.source.find("bennu_print_integer") == std::string::npos,
        "array emission omits unused integer formatting");
}

void test_emission_returns_the_shared_semantic_error() {
  const bennu::CEmissionResult result =
      bennu::emit_c_source("inc 5\ninc 9223372036854775807\n");

  check(!result.ok, "semantically invalid source is not emitted");
  check(result.source.empty(), "failed emission returns no partial C source");
  check(result.error.kind == bennu::ErrorKind::integer_overflow,
        "emission preserves the evaluator error category");
  check(result.error.location.line == 2 && result.error.location.column == 1,
        "emission preserves the evaluator source location");
}

} // namespace

int main() {
  test_emission_is_deterministic_and_specializes_used_value_kinds();
  test_emission_returns_the_shared_semantic_error();
  return failures == 0 ? 0 : 1;
}
