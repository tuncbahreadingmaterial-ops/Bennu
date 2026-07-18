#ifndef BENNU_EVALUATOR_HPP
#define BENNU_EVALUATOR_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bennu {

enum class ValueKind {
  integer,
  array,
};

inline constexpr std::int64_t ioata_element_limit = 1'000'000;

struct SourceLocation {
  std::size_t offset;
  std::size_t line;
  std::size_t column;
};

enum class ErrorKind {
  none,
  illegal_character,
  malformed_integer,
  integer_out_of_range,
  unknown_name,
  missing_argument,
  expected_whitespace,
  trailing_token,
  integer_overflow,
  type_mismatch,
  allocation_limit_exceeded,
  empty_expression,
  invalid_program,
};

struct Error {
  ErrorKind kind;
  SourceLocation location;
  std::string message;
};

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

struct Value {
  ValueKind kind;
  std::int64_t integer;
  std::vector<std::int64_t> elements;
};

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
TokenizeResult tokenize(std::string_view source);
ParseResult parse_program(std::string_view source,
                          const std::vector<Token> &tokens);
ValueResult apply_inc(const Value &argument, SourceLocation location);
ValueResult apply_ioata(const Value &argument, SourceLocation location);
ProgramResult evaluate_program(const Program &program);
std::string format_value(const Value &value);

} // namespace bennu

#endif
