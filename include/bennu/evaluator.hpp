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
};

struct Error {
  ErrorKind kind;
  SourceLocation location;
  std::string message;
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
std::string format_value(const Value &value);

} // namespace bennu

#endif
