#include "bennu/c_emitter.hpp"

#include "doctest/doctest.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

namespace bennu {

namespace {

void append_integer_constant(std::string &output, std::int64_t value) {
  if (value == INT64_MIN) {
    output += "(-INT64_C(9223372036854775807) - INT64_C(1))";
    return;
  }

  const bool negative = value < 0;
  if (negative) {
    output += "(-INT64_C(";
    value = -value;
  } else {
    output += "INT64_C(";
  }

  std::array<char, 32> digits{};
  const std::to_chars_result converted =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  output.append(digits.data(), converted.ptr);
  output += negative ? "))" : ")";
}

} // namespace

CEmissionResult emit_c_source(std::string_view source) {
  ProgramResult evaluated = evaluate_source(source);
  if (!evaluated.ok) {
    return CEmissionResult{false, {}, std::move(evaluated.error)};
  }

  bool has_integer = false;
  bool has_array = false;
  for (const Value &value : evaluated.values) {
    has_integer = has_integer || value.kind == ValueKind::integer;
    has_array = has_array || value.kind == ValueKind::array;
  }

  std::string output =
      "/* Generated deterministically by Bennu. Standard C11. */\n"
      "#include <inttypes.h>\n"
      "#include <stdint.h>\n"
      "#include <stdio.h>\n\n";
  if (has_integer) {
    output +=
        "static int bennu_print_integer(int64_t value) {\n"
        "  return fprintf(stdout, \">>%\" PRId64 \"\\n\", value) < 0;\n"
        "}\n\n";
  }
  if (has_array) {
    output +=
        "static int bennu_print_array(int64_t count) {\n"
        "  if (fputs(\">>(\", stdout) == EOF) {\n"
        "    return 1;\n"
        "  }\n"
        "  for (int64_t value = INT64_C(1); value <= count; ++value) {\n"
        "    if (value != INT64_C(1) && fputc(' ', stdout) == EOF) {\n"
        "      return 1;\n"
        "    }\n"
        "    if (fprintf(stdout, \"%\" PRId64, value) < 0) {\n"
        "      return 1;\n"
        "    }\n"
        "  }\n"
        "  return fputs(\")\\n\", stdout) == EOF;\n"
        "}\n\n";
  }
  output += "int main(void) {\n";

  for (const Value &value : evaluated.values) {
    if (value.kind == ValueKind::integer) {
      output += "  if (bennu_print_integer(";
      append_integer_constant(output, value.integer);
    } else {
      output += "  if (bennu_print_array(";
      append_integer_constant(
          output, static_cast<std::int64_t>(value.elements.size()));
    }
    output += ") != 0) {\n    return 1;\n  }\n";
  }
  output += "  return fflush(stdout) == 0 ? 0 : 1;\n}\n";

  return CEmissionResult{
      true,
      std::move(output),
      Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
  };
}

TEST_CASE("C emission is deterministic and specializes used value kinds") {
  const CEmissionResult scalar = emit_c_source("inc 5");
  const CEmissionResult scalar_repeat = emit_c_source("inc 5");
  const CEmissionResult array = emit_c_source("ioata 0");

  REQUIRE(scalar.ok);
  REQUIRE(scalar_repeat.ok);
  CHECK(scalar.source == scalar_repeat.source);
  CHECK(scalar.source.find("bennu_print_integer") != std::string::npos);
  CHECK(scalar.source.find("bennu_print_array") == std::string::npos);
  REQUIRE(array.ok);
  CHECK(array.source.find("bennu_print_array") != std::string::npos);
  CHECK(array.source.find("bennu_print_integer") == std::string::npos);
}

TEST_CASE("C emission returns the shared semantic error without partial source") {
  const CEmissionResult result =
      emit_c_source("inc 5\ninc 9223372036854775807\n");

  CHECK_FALSE(result.ok);
  CHECK(result.source.empty());
  CHECK(result.error.kind == ErrorKind::integer_overflow);
  CHECK(result.error.location.line == 2);
  CHECK(result.error.location.column == 1);
}

} // namespace bennu
