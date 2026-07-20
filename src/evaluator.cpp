#include "bennu/evaluator.hpp"

#include "doctest/doctest.h"

#include <array>
#include <charconv>
#include <limits>
#include <ostream>
#include <utility>

namespace bennu {

namespace {

constexpr std::int64_t ioata_element_limit = 1'000'000;

enum class TokenKind {
  integer,
  inc,
  ioata,
  name,
  newline,
  end,
};

struct Token {
  TokenKind kind;
  SourceLocation location;
  std::size_t length;
  bool separated;
};

struct TokenizeResult {
  bool ok;
  std::vector<Token> tokens;
  Error error;
};

enum class ExpressionKind {
  integer,
  inc,
  ioata,
};

struct ExpressionNode {
  ExpressionKind kind;
  SourceLocation location;
  std::int64_t integer;
  std::size_t argument;
};

struct Program {
  std::vector<ExpressionNode> nodes;
  std::vector<std::size_t> roots;
};

struct ParseResult {
  bool ok;
  Program program;
  Error error;
};

bool is_ascii_letter(char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z');
}

bool is_ascii_digit(char character) {
  return character >= '0' && character <= '9';
}

bool is_name_character(char character) {
  return is_ascii_letter(character) || is_ascii_digit(character) ||
         character == '_';
}

TokenizeResult tokenize(std::string_view source) {
  std::vector<Token> tokens;
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
  bool separated = true;

  while (offset < source.size()) {
    const char character = source[offset];
    if (character == ' ' || character == '\t') {
      ++offset;
      ++column;
      separated = true;
      continue;
    }

    const SourceLocation location{offset, line, column};
    if (character == '\n' || character == '\r') {
      if (character == '\r' &&
          (offset + 1 >= source.size() || source[offset + 1] != '\n')) {
        return TokenizeResult{
            false,
            std::move(tokens),
            Error{ErrorKind::illegal_character, location,
                  "carriage return must be followed by line feed"},
        };
      }
      const std::size_t length = character == '\r' ? 2 : 1;
      tokens.push_back(Token{TokenKind::newline, location, length, separated});
      offset += length;
      ++line;
      column = 1;
      separated = true;
      continue;
    }

    if (character == '-' || is_ascii_digit(character)) {
      const std::size_t start = offset;
      if (character == '-') {
        ++offset;
        ++column;
        if (offset >= source.size() || !is_ascii_digit(source[offset])) {
          return TokenizeResult{
              false,
              std::move(tokens),
              Error{ErrorKind::malformed_integer, location,
                    "'-' must be followed by decimal digits"},
          };
        }
      }
      while (offset < source.size() && is_ascii_digit(source[offset])) {
        ++offset;
        ++column;
      }
      if (offset < source.size() && is_name_character(source[offset])) {
        while (offset < source.size() && is_name_character(source[offset])) {
          ++offset;
          ++column;
        }
        return TokenizeResult{
            false,
            std::move(tokens),
            Error{ErrorKind::malformed_integer, location,
                  "integer contains non-decimal characters"},
        };
      }
      tokens.push_back(
          Token{TokenKind::integer, location, offset - start, separated});
      separated = false;
      continue;
    }

    if (is_ascii_letter(character)) {
      const std::size_t start = offset;
      while (offset < source.size() && is_name_character(source[offset])) {
        ++offset;
        ++column;
      }
      const std::string_view name = source.substr(start, offset - start);
      TokenKind kind = TokenKind::name;
      if (name == "inc") {
        kind = TokenKind::inc;
      } else if (name == "ioata") {
        kind = TokenKind::ioata;
      }
      tokens.push_back(Token{kind, location, offset - start, separated});
      separated = false;
      continue;
    }

    return TokenizeResult{
        false,
        std::move(tokens),
        Error{ErrorKind::illegal_character, location,
              "illegal character in source"},
    };
  }

  tokens.push_back(
      Token{TokenKind::end, SourceLocation{offset, line, column}, 0, separated});
  return TokenizeResult{
      true,
      std::move(tokens),
      Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
  };
}

ParseResult parse_program(std::string_view source,
                          const std::vector<Token> &tokens) {
  Program program;
  std::size_t cursor = 0;
  while (cursor < tokens.size()) {
    if (tokens[cursor].kind == TokenKind::newline) {
      ++cursor;
      continue;
    }
    if (tokens[cursor].kind == TokenKind::end) {
      return ParseResult{
          true,
          std::move(program),
          Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
      };
    }

    const std::size_t line_start = cursor;
    std::size_t line_end = cursor;
    while (line_end < tokens.size() &&
           tokens[line_end].kind != TokenKind::newline &&
           tokens[line_end].kind != TokenKind::end) {
      ++line_end;
    }

    std::size_t base = line_start;
    while (base < line_end &&
           (tokens[base].kind == TokenKind::inc ||
            tokens[base].kind == TokenKind::ioata)) {
      if (base + 1 == line_end) {
        return ParseResult{
            false,
            {},
            Error{ErrorKind::missing_argument, tokens[base].location,
                  "primitive is missing its argument"},
        };
      }
      if (!tokens[base + 1].separated) {
        return ParseResult{
            false,
            {},
            Error{ErrorKind::expected_whitespace, tokens[base + 1].location,
                  "primitive name and argument require whitespace"},
        };
      }
      ++base;
    }

    if (tokens[base].kind == TokenKind::name) {
      return ParseResult{
          false,
          {},
          Error{ErrorKind::unknown_name, tokens[base].location,
                "unknown name: " +
                    std::string(source.substr(tokens[base].location.offset,
                                              tokens[base].length))},
      };
    }
    if (tokens[base].kind != TokenKind::integer) {
      return ParseResult{
          false,
          {},
          Error{ErrorKind::missing_argument, tokens[base].location,
                "expression requires an integer argument"},
      };
    }
    if (base + 1 != line_end) {
      return ParseResult{
          false,
          {},
          Error{ErrorKind::trailing_token, tokens[base + 1].location,
                "expression has trailing input"},
      };
    }

    const Token &integer_token = tokens[base];
    const std::string_view integer_source = source.substr(
        integer_token.location.offset, integer_token.length);
    std::int64_t integer = 0;
    const std::from_chars_result converted =
        std::from_chars(integer_source.data(),
                        integer_source.data() + integer_source.size(), integer);
    if (converted.ec == std::errc::result_out_of_range) {
      return ParseResult{
          false,
          {},
          Error{ErrorKind::integer_out_of_range, integer_token.location,
                "integer literal is outside the signed 64-bit range"},
      };
    }
    if (converted.ec != std::errc{} ||
        converted.ptr != integer_source.data() + integer_source.size()) {
      return ParseResult{
          false,
          {},
          Error{ErrorKind::malformed_integer, integer_token.location,
                "malformed decimal integer"},
      };
    }

    std::size_t argument = program.nodes.size();
    program.nodes.push_back(ExpressionNode{ExpressionKind::integer,
                                           integer_token.location, integer, 0});
    for (std::size_t index = base; index-- > line_start;) {
      const ExpressionKind kind = tokens[index].kind == TokenKind::inc
                                      ? ExpressionKind::inc
                                      : ExpressionKind::ioata;
      const std::size_t node_index = program.nodes.size();
      program.nodes.push_back(
          ExpressionNode{kind, tokens[index].location, 0, argument});
      argument = node_index;
    }
    program.roots.push_back(argument);
    cursor = line_end;
  }

  return ParseResult{
      true,
      std::move(program),
      Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
  };
}

ValueResult apply_inc(const Value &argument, SourceLocation location) {
  if (argument.container != ContainerKind::scalar ||
      argument.scalar.type != ScalarType::integer) {
    Error error = make_error(ErrorKind::type_mismatch, location,
                             "inc requires a scalar integer argument");
    error.primitive = PrimitiveErrorContext{"inc"};
    error.argument_position = 1;
    return ValueResult{
        false,
        make_int_value(0),
        std::move(error),
    };
  }
  if (argument.scalar.integer == std::numeric_limits<std::int64_t>::max()) {
    Error error = make_error(
        ErrorKind::integer_overflow, location,
        "inc result exceeds the signed 64-bit range");
    error.primitive = PrimitiveErrorContext{"inc"};
    error.argument_position = 1;
    error.domain = DomainErrorContext{
        DomainErrorReason::integer_overflow,
        ScalarSignatureContext{{ScalarType::integer}, ScalarType::integer},
        {argument.scalar},
    };
    return ValueResult{
        false,
        make_int_value(0),
        std::move(error),
    };
  }
  return ValueResult{
      true,
      make_int_value(argument.scalar.integer + 1),
      make_error(ErrorKind::none, SourceLocation{0, 1, 1}, ""),
  };
}

ValueResult apply_ioata(const Value &argument, SourceLocation location,
                        std::int64_t remaining_elements) {
  if (argument.container != ContainerKind::scalar ||
      argument.scalar.type != ScalarType::integer) {
    Error error = make_error(ErrorKind::type_mismatch, location,
                             "ioata requires a scalar integer argument");
    error.primitive = PrimitiveErrorContext{"ioata"};
    error.argument_position = 1;
    return ValueResult{
        false,
        make_int_value(0),
        std::move(error),
    };
  }
  if (argument.scalar.integer > remaining_elements) {
    Error error = make_error(
        ErrorKind::allocation_limit_exceeded, location,
        "program ioata results exceed the Level 1 element limit");
    error.primitive = PrimitiveErrorContext{"ioata"};
    error.argument_position = 1;
    const std::size_t requested =
        static_cast<std::size_t>(argument.scalar.integer);
    error.resource = ResourceErrorContext{
        ResourceErrorReason::profile_limit,
        requested,
        std::nullopt,
        "bootstrap-level1",
        std::nullopt,
        static_cast<std::size_t>(ioata_element_limit),
        static_cast<std::size_t>(ioata_element_limit - remaining_elements),
        requested,
    };
    return ValueResult{
        false,
        make_int_value(0),
        std::move(error),
    };
  }

  std::vector<std::int64_t> elements;
  if (argument.scalar.integer > 0) {
    elements.reserve(static_cast<std::size_t>(argument.scalar.integer));
    for (std::int64_t value = 1; value <= argument.scalar.integer; ++value) {
      elements.push_back(value);
    }
  }
  ValueConstructionResult constructed = make_int_vector(std::move(elements));
  return ValueResult{
      true,
      std::move(constructed.value),
      make_error(ErrorKind::none, SourceLocation{0, 1, 1}, ""),
  };
}

ProgramResult evaluate_program(const Program &program) {
  std::vector<Value> node_values;
  node_values.reserve(program.nodes.size());
  std::int64_t remaining_ioata_elements = ioata_element_limit;
  for (std::size_t index = 0; index < program.nodes.size(); ++index) {
    const ExpressionNode &node = program.nodes[index];
    if (node.kind == ExpressionKind::integer) {
      node_values.push_back(make_int_value(node.integer));
      continue;
    }
    const Value &argument = node_values[node.argument];

    if (node.kind == ExpressionKind::inc) {
      ValueResult result = apply_inc(argument, node.location);
      if (!result.ok) {
        return ProgramResult{false, {}, std::move(result.error)};
      }
      node_values.push_back(std::move(result.value));
      continue;
    }

    ValueResult result =
        apply_ioata(argument, node.location, remaining_ioata_elements);
    if (!result.ok) {
      return ProgramResult{false, {}, std::move(result.error)};
    }
    remaining_ioata_elements -=
        static_cast<std::int64_t>(value_length(result.value));
    node_values.push_back(std::move(result.value));
  }

  std::vector<Value> results;
  results.reserve(program.roots.size());
  for (const std::size_t root : program.roots) {
    results.push_back(std::move(node_values[root]));
  }
  return ProgramResult{
      true,
      std::move(results),
      Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
  };
}

} // namespace

ValueResult evaluate_expression(std::string_view source) {
  const TokenizeResult tokenized = tokenize(source);
  if (!tokenized.ok) {
    return ValueResult{false, make_int_value(0), tokenized.error};
  }
  const ParseResult parsed = parse_program(source, tokenized.tokens);
  if (!parsed.ok) {
    return ValueResult{false, make_int_value(0), parsed.error};
  }
  if (parsed.program.roots.empty()) {
    return ValueResult{
        false,
        make_int_value(0),
        Error{ErrorKind::empty_expression, tokenized.tokens.back().location,
              "expected one expression"},
    };
  }
  if (parsed.program.roots.size() != 1) {
    const SourceLocation location =
        parsed.program.nodes[parsed.program.roots[1]].location;
    return ValueResult{
        false,
        make_int_value(0),
        Error{ErrorKind::trailing_token, location,
              "expected one expression, found a complete program"},
    };
  }
  ProgramResult evaluated = evaluate_program(parsed.program);
  if (!evaluated.ok) {
    return ValueResult{false, make_int_value(0), std::move(evaluated.error)};
  }
  return ValueResult{
      true,
      std::move(evaluated.values[0]),
      Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
  };
}

ProgramResult evaluate_source(std::string_view source) {
  const TokenizeResult tokenized = tokenize(source);
  if (!tokenized.ok) {
    return ProgramResult{false, {}, tokenized.error};
  }
  const ParseResult parsed = parse_program(source, tokenized.tokens);
  if (!parsed.ok) {
    return ProgramResult{false, {}, parsed.error};
  }
  return evaluate_program(parsed.program);
}

TEST_CASE("integer expressions preserve signed 64-bit values") {
  const ValueResult ordinary = evaluate_expression("5");
  const ValueResult minimum =
      evaluate_expression("-9223372036854775808");
  const ValueResult maximum = evaluate_expression("9223372036854775807");

  REQUIRE(ordinary.ok);
  CHECK(ordinary.value.container == ContainerKind::scalar);
  CHECK(ordinary.value.scalar.type == ScalarType::integer);
  CHECK(ordinary.value.scalar.integer == std::int64_t{5});
  REQUIRE(minimum.ok);
  CHECK(minimum.value.scalar.integer ==
        std::numeric_limits<std::int64_t>::min());
  REQUIRE(maximum.ok);
  CHECK(maximum.value.scalar.integer ==
        std::numeric_limits<std::int64_t>::max());
}

TEST_CASE("Level 1 primitives and value formatting remain exact") {
  const ValueResult incremented = evaluate_expression("inc 5");
  const ValueResult array = evaluate_expression("ioata 5");
  const ValueResult zero = evaluate_expression("ioata 0");
  const ValueResult negative = evaluate_expression("ioata -5");

  REQUIRE(incremented.ok);
  CHECK(incremented.value.container == ContainerKind::scalar);
  CHECK(incremented.value.scalar.type == ScalarType::integer);
  CHECK(incremented.value.scalar.integer == std::int64_t{6});
  CHECK(format_value(incremented.value) == "6");

  REQUIRE(array.ok);
  CHECK(array.value.container == ContainerKind::vector);
  CHECK(array.value.vector.element_type == ScalarType::integer);
  CHECK(array.value.vector.integers ==
        std::vector<std::int64_t>{1, 2, 3, 4, 5});
  CHECK(format_value(array.value) == "(1 2 3 4 5)");

  REQUIRE(zero.ok);
  CHECK(format_value(zero.value) == "()");
  REQUIRE(negative.ok);
  CHECK(format_value(negative.value) == "()");
}

TEST_CASE("program evaluation preserves source order and accepted whitespace") {
  const ProgramResult ordered = evaluate_source("ioata 5\ninc 5");
  REQUIRE(ordered.ok);
  REQUIRE(ordered.values.size() == 2);
  CHECK(format_value(ordered.values[0]) == "(1 2 3 4 5)");
  CHECK(format_value(ordered.values[1]) == "6");

  const ProgramResult mixed =
      evaluate_source(" \tioata 2 \r\n\r\n\tinc 5\t\n\n-1");
  REQUIRE(mixed.ok);
  REQUIRE(mixed.values.size() == 3);
  CHECK(format_value(mixed.values[0]) == "(1 2)");
  CHECK(format_value(mixed.values[1]) == "6");
  CHECK(format_value(mixed.values[2]) == "-1");

  const ProgramResult empty = evaluate_source(" \t\r\n\n");
  CHECK(empty.ok);
  CHECK(empty.values.empty());
}

TEST_CASE("internal tokenizer preserves kinds and source locations") {
  const TokenizeResult result = tokenize(" \tioata inc -5\r\n");

  REQUIRE(result.ok);
  REQUIRE(result.tokens.size() == 5);
  CHECK(result.tokens[0].kind == TokenKind::ioata);
  CHECK(result.tokens[0].location.line == 1);
  CHECK(result.tokens[0].location.column == 3);
  CHECK(result.tokens[1].kind == TokenKind::inc);
  CHECK(result.tokens[2].kind == TokenKind::integer);
  CHECK(result.tokens[3].kind == TokenKind::newline);
  CHECK(result.tokens[3].length == 2);
  CHECK(result.tokens[4].kind == TokenKind::end);
  CHECK(result.tokens[4].location.line == 2);
  CHECK(result.tokens[4].location.column == 1);
}

TEST_CASE("internal parser builds flat postorder nodes for nested calls") {
  constexpr std::string_view source = "ioata inc 5";
  const TokenizeResult tokenized = tokenize(source);
  REQUIRE(tokenized.ok);

  const ParseResult parsed = parse_program(source, tokenized.tokens);
  REQUIRE(parsed.ok);
  REQUIRE(parsed.program.nodes.size() == 3);
  REQUIRE(parsed.program.roots.size() == 1);
  CHECK(parsed.program.roots[0] == 2);
  CHECK(parsed.program.nodes[0].kind == ExpressionKind::integer);
  CHECK(parsed.program.nodes[1].kind == ExpressionKind::inc);
  CHECK(parsed.program.nodes[1].argument == 0);
  CHECK(parsed.program.nodes[2].kind == ExpressionKind::ioata);
  CHECK(parsed.program.nodes[2].argument == 1);
}

TEST_CASE("internal flat evaluator handles nested prefix calls") {
  constexpr std::string_view source = "ioata inc 5";
  const TokenizeResult tokenized = tokenize(source);
  REQUIRE(tokenized.ok);
  const ParseResult parsed = parse_program(source, tokenized.tokens);
  REQUIRE(parsed.ok);

  const ProgramResult evaluated = evaluate_program(parsed.program);
  REQUIRE(evaluated.ok);
  REQUIRE(evaluated.values.size() == 1);
  CHECK(format_value(evaluated.values[0]) == "(1 2 3 4 5 6)");
}

TEST_CASE("internal primitive kernels preserve semantic errors") {
  const SourceLocation location{17, 3, 4};
  const Value maximum =
      make_int_value(std::numeric_limits<std::int64_t>::max());
  const Value array = make_int_vector({1, 2, 3}).value;

  const ValueResult overflow = apply_inc(maximum, location);
  const ValueResult wrong_inc_type = apply_inc(array, location);
  const ValueResult wrong_ioata_type =
      apply_ioata(array, location, ioata_element_limit);

  CHECK_FALSE(overflow.ok);
  CHECK(overflow.error.kind == ErrorKind::integer_overflow);
  CHECK(overflow.error.location.offset == 17);
  CHECK(overflow.error.location.line == 3);
  CHECK(overflow.error.location.column == 4);
  REQUIRE(overflow.error.primitive.has_value());
  CHECK(overflow.error.primitive->name == "inc");
  REQUIRE(overflow.error.argument_position.has_value());
  CHECK(*overflow.error.argument_position == 1);
  REQUIRE(overflow.error.domain.has_value());
  CHECK(overflow.error.domain->reason ==
        DomainErrorReason::integer_overflow);
  REQUIRE(overflow.error.domain->operands.size() == 1);
  CHECK(overflow.error.domain->operands[0].integer ==
        std::numeric_limits<std::int64_t>::max());
  CHECK_FALSE(wrong_inc_type.ok);
  CHECK(wrong_inc_type.error.kind == ErrorKind::type_mismatch);
  REQUIRE(wrong_inc_type.error.primitive.has_value());
  CHECK(wrong_inc_type.error.primitive->name == "inc");
  CHECK_FALSE(wrong_ioata_type.ok);
  CHECK(wrong_ioata_type.error.kind == ErrorKind::type_mismatch);
  REQUIRE(wrong_ioata_type.error.primitive.has_value());
  CHECK(wrong_ioata_type.error.primitive->name == "ioata");
}

TEST_CASE("ioata enforces one pre-allocation retained-element budget") {
  const SourceLocation location{6, 1, 7};
  const Value request = make_int_value(ioata_element_limit + 1);
  const Value boundary = make_int_value(ioata_element_limit);

  const ValueResult rejected =
      apply_ioata(request, location, ioata_element_limit);
  CHECK_FALSE(rejected.ok);
  CHECK(rejected.error.kind == ErrorKind::allocation_limit_exceeded);
  CHECK(rejected.value.container == ContainerKind::scalar);
  CHECK(rejected.error.location.offset == 6);
  CHECK(rejected.error.location.line == 1);
  CHECK(rejected.error.location.column == 7);
  REQUIRE(rejected.error.primitive.has_value());
  CHECK(rejected.error.primitive->name == "ioata");
  REQUIRE(rejected.error.resource.has_value());
  CHECK(rejected.error.resource->reason == ResourceErrorReason::profile_limit);
  REQUIRE(rejected.error.resource->requested_elements.has_value());
  CHECK(*rejected.error.resource->requested_elements ==
        static_cast<std::size_t>(ioata_element_limit + 1));
  CHECK(rejected.error.resource->profile == "bootstrap-level1");

  const ValueResult allowed =
      apply_ioata(boundary, location, ioata_element_limit);
  REQUIRE(allowed.ok);
  REQUIRE(allowed.value.vector.integers.size() ==
          static_cast<std::size_t>(ioata_element_limit));
  CHECK(allowed.value.vector.integers.back() == ioata_element_limit);

  const ValueResult no_remaining =
      apply_ioata(make_int_value(1), location, 0);
  CHECK_FALSE(no_remaining.ok);
  CHECK(no_remaining.error.kind == ErrorKind::allocation_limit_exceeded);
}

TEST_CASE("complete programs cap cumulative retained ioata elements") {
  const ProgramResult result =
      evaluate_source("ioata 600000\nioata 600000");

  CHECK_FALSE(result.ok);
  CHECK(result.error.kind == ErrorKind::allocation_limit_exceeded);
  CHECK(result.error.location.line == 2);
  CHECK(result.error.location.column == 1);
}

TEST_CASE("source-reachable failures retain categories and locations") {
  struct ErrorCase {
    std::string_view source;
    ErrorKind kind;
    std::size_t offset;
    std::size_t line;
    std::size_t column;
  };
  const std::array<ErrorCase, 11> cases{{
      {"\n\twat", ErrorKind::unknown_name, 2, 2, 2},
      {"ioata @", ErrorKind::illegal_character, 6, 1, 7},
      {"inc", ErrorKind::missing_argument, 0, 1, 1},
      {"5 extra", ErrorKind::trailing_token, 2, 1, 3},
      {"12x", ErrorKind::malformed_integer, 0, 1, 1},
      {"-9223372036854775809", ErrorKind::integer_out_of_range, 0, 1, 1},
      {"9223372036854775808", ErrorKind::integer_out_of_range, 0, 1, 1},
      {"inc 9223372036854775807", ErrorKind::integer_overflow, 0, 1, 1},
      {"\n  inc ioata 3", ErrorKind::type_mismatch, 3, 2, 3},
      {"ioata 1000001", ErrorKind::allocation_limit_exceeded, 0, 1, 1},
      {"inc-5", ErrorKind::expected_whitespace, 3, 1, 4},
  }};

  for (const ErrorCase &error_case : cases) {
    CAPTURE(error_case.source);
    const ValueResult result = evaluate_expression(error_case.source);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == error_case.kind);
    CHECK(result.error.location.offset == error_case.offset);
    CHECK(result.error.location.line == error_case.line);
    CHECK(result.error.location.column == error_case.column);
    CHECK_FALSE(result.error.message.empty());
  }
}

TEST_CASE("one-expression API locates empty input at the source end") {
  const ValueResult result = evaluate_expression("\r\n\t");

  CHECK_FALSE(result.ok);
  CHECK(result.error.kind == ErrorKind::empty_expression);
  CHECK(result.error.location.offset == 3);
  CHECK(result.error.location.line == 2);
  CHECK(result.error.location.column == 2);
}

} // namespace bennu
