#include "bennu/evaluator.hpp"

#include <charconv>
#include <limits>
#include <utility>

namespace bennu {

namespace {

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

} // namespace

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
  for (const Token &token : tokens) {
    if (token.location.offset > source.size() ||
        token.length > source.size() - token.location.offset) {
      return ParseResult{
          false,
          {},
          Error{ErrorKind::invalid_program, token.location,
                "token span is outside the source buffer"},
      };
    }
  }

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
  if (argument.kind != ValueKind::integer) {
    return ValueResult{
        false,
        Value{ValueKind::integer, 0, {}},
        Error{ErrorKind::type_mismatch, location,
              "inc requires a scalar integer argument"},
    };
  }
  if (argument.integer == std::numeric_limits<std::int64_t>::max()) {
    return ValueResult{
        false,
        Value{ValueKind::integer, 0, {}},
        Error{ErrorKind::integer_overflow, location,
              "inc result exceeds the signed 64-bit range"},
    };
  }
  return ValueResult{
      true,
      Value{ValueKind::integer, argument.integer + 1, {}},
      Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
  };
}

ValueResult apply_ioata(const Value &argument, SourceLocation location) {
  if (argument.kind != ValueKind::integer) {
    return ValueResult{
        false,
        Value{ValueKind::integer, 0, {}},
        Error{ErrorKind::type_mismatch, location,
              "ioata requires a scalar integer argument"},
    };
  }
  if (argument.integer > ioata_element_limit) {
    return ValueResult{
        false,
        Value{ValueKind::integer, 0, {}},
        Error{ErrorKind::allocation_limit_exceeded, location,
              "ioata request exceeds the Level 1 element limit"},
    };
  }

  std::vector<std::int64_t> elements;
  if (argument.integer > 0) {
    elements.reserve(static_cast<std::size_t>(argument.integer));
    for (std::int64_t value = 1; value <= argument.integer; ++value) {
      elements.push_back(value);
    }
  }
  return ValueResult{
      true,
      Value{ValueKind::array, 0, std::move(elements)},
      Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
  };
}

ProgramResult evaluate_program(const Program &program) {
  std::vector<Value> node_values;
  node_values.reserve(program.nodes.size());
  std::int64_t ioata_elements = 0;
  for (std::size_t index = 0; index < program.nodes.size(); ++index) {
    const ExpressionNode &node = program.nodes[index];
    if (node.kind == ExpressionKind::integer) {
      node_values.push_back(Value{ValueKind::integer, node.integer, {}});
      continue;
    }
    if (node.kind != ExpressionKind::inc &&
        node.kind != ExpressionKind::ioata) {
      return ProgramResult{
          false,
          {},
          Error{ErrorKind::invalid_program, node.location,
                "expression has an unknown kind"},
      };
    }
    if (node.argument >= index) {
      return ProgramResult{
          false,
          {},
          Error{ErrorKind::invalid_program, node.location,
                "expression argument must refer to an earlier node"},
      };
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

    if (argument.kind == ValueKind::integer && argument.integer > 0 &&
        argument.integer > ioata_element_limit - ioata_elements) {
      return ProgramResult{
          false,
          {},
          Error{ErrorKind::allocation_limit_exceeded, node.location,
                "program ioata results exceed the Level 1 element limit"},
      };
    }
    ValueResult result = apply_ioata(argument, node.location);
    if (!result.ok) {
      return ProgramResult{false, {}, std::move(result.error)};
    }
    ioata_elements += static_cast<std::int64_t>(result.value.elements.size());
    node_values.push_back(std::move(result.value));
  }

  std::vector<Value> results;
  results.reserve(program.roots.size());
  std::size_t previous_root = 0;
  bool has_previous_root = false;
  for (const std::size_t root : program.roots) {
    if (root >= node_values.size() ||
        (has_previous_root && root <= previous_root)) {
      const SourceLocation location = root < program.nodes.size()
                                          ? program.nodes[root].location
                                          : SourceLocation{0, 1, 1};
      return ProgramResult{
          false,
          {},
          Error{ErrorKind::invalid_program, location,
                "program roots must be unique nodes in source order"},
      };
    }
    results.push_back(std::move(node_values[root]));
    previous_root = root;
    has_previous_root = true;
  }
  return ProgramResult{
      true,
      std::move(results),
      Error{ErrorKind::none, SourceLocation{0, 1, 1}, ""},
  };
}

ValueResult evaluate_expression(std::string_view source) {
  const TokenizeResult tokenized = tokenize(source);
  if (!tokenized.ok) {
    return ValueResult{false, Value{ValueKind::integer, 0, {}},
                       tokenized.error};
  }
  const ParseResult parsed = parse_program(source, tokenized.tokens);
  if (!parsed.ok) {
    return ValueResult{false, Value{ValueKind::integer, 0, {}}, parsed.error};
  }
  if (parsed.program.roots.empty()) {
    return ValueResult{
        false,
        Value{ValueKind::integer, 0, {}},
        Error{ErrorKind::empty_expression, tokenized.tokens.back().location,
              "expected one expression"},
    };
  }
  if (parsed.program.roots.size() != 1) {
    const SourceLocation location =
        parsed.program.nodes[parsed.program.roots[1]].location;
    return ValueResult{
        false,
        Value{ValueKind::integer, 0, {}},
        Error{ErrorKind::trailing_token, location,
              "expected one expression, found a complete program"},
    };
  }
  ProgramResult evaluated = evaluate_program(parsed.program);
  if (!evaluated.ok) {
    return ValueResult{false, Value{ValueKind::integer, 0, {}},
                       std::move(evaluated.error)};
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

std::string format_value(const Value &value) {
  if (value.kind == ValueKind::integer) {
    return std::to_string(value.integer);
  }

  std::string formatted = "(";
  for (std::size_t index = 0; index < value.elements.size(); ++index) {
    if (index != 0) {
      formatted += ' ';
    }
    formatted += std::to_string(value.elements[index]);
  }
  formatted += ')';
  return formatted;
}

} // namespace bennu
