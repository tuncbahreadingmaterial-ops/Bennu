#include "bennu/application.hpp"
#include "bennu/c_emitter.hpp"
#include "bennu/evaluator.hpp"
#include "bennu/primitive.hpp"
#include "bennu/resources.hpp"
#include "bennu/value.hpp"
#include "rewrite_c_runtime.hpp"
#include "typed_application.hpp"

#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <bit>
#ifndef DOCTEST_CONFIG_DISABLE
#include <ostream>
#endif
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(BENNU_REWRITE_STRTOD_L)
#include <cerrno>
#include <cfenv>
#include <cstdlib>
#include <locale.h>
#endif

namespace bennu {
namespace {

struct RewritePosition {
  std::size_t offset;
  std::size_t line;
  std::size_t column;
};

struct RewriteSpan {
  RewritePosition begin;
  RewritePosition end;
};

enum class RewriteTokenKind {
  name,
  bool_literal,
  int_literal,
  double_literal,
  bool_type,
  int_type,
  double_type,
  left_bracket,
  right_bracket,
  left_parenthesis,
  right_parenthesis,
  horizontal_space,
  line_terminator,
  malformed_literal,
  invalid,
};

enum class RewriteLiteralError {
  none,
  malformed,
  range,
};

struct RewriteToken {
  RewriteTokenKind kind;
  RewriteSpan span;
  bool boolean;
  std::int64_t integer;
  double double_precision;
  RewriteLiteralError literal_error;
};

struct RewriteTokens {
  std::string source;
  std::vector<RewriteToken> tokens;
  RewritePosition end;
};

bool is_lowercase(char byte) { return byte >= 'a' && byte <= 'z'; }

bool is_uppercase(char byte) { return byte >= 'A' && byte <= 'Z'; }

bool is_digit(char byte) { return byte >= '0' && byte <= '9'; }

bool is_horizontal_space(char byte) { return byte == ' ' || byte == '\t'; }

bool is_numeric_candidate_byte(char byte) {
  return is_lowercase(byte) || is_uppercase(byte) || is_digit(byte) ||
         byte == '_' || byte == '.' || byte == '+' || byte == '-';
}

RewriteToken make_token(RewriteTokenKind kind, RewritePosition begin,
                        RewritePosition end) {
  return RewriteToken{kind,
                      RewriteSpan{begin, end},
                      false,
                      0,
                      0.0,
                      RewriteLiteralError::none};
}

bool canonical_integer_grammar(std::string_view spelling) {
  std::size_t digit = 0U;
  if (!spelling.empty() && spelling.front() == '-') {
    digit = 1U;
  }
  if (digit == spelling.size()) {
    return false;
  }
  if (spelling[digit] == '0') {
    return spelling.size() == digit + 1U && digit == 0U;
  }
  if (spelling[digit] < '1' || spelling[digit] > '9') {
    return false;
  }
  for (std::size_t index = digit + 1U; index < spelling.size(); ++index) {
    if (!is_digit(spelling[index])) {
      return false;
    }
  }
  return true;
}

bool parse_canonical_integer(std::string_view spelling, std::int64_t &value) {
  if (!canonical_integer_grammar(spelling)) {
    return false;
  }
  const auto converted =
      std::from_chars(spelling.data(), spelling.data() + spelling.size(), value);
  return converted.ec == std::errc{} &&
         converted.ptr == spelling.data() + spelling.size();
}

bool finite_double_grammar(std::string_view spelling) {
  std::size_t index = 0U;
  if (!spelling.empty() && spelling.front() == '-') {
    index = 1U;
  }
  if (index == spelling.size()) {
    return false;
  }
  const std::size_t integer_begin = index;
  while (index < spelling.size() && is_digit(spelling[index])) {
    ++index;
  }
  if (integer_begin == index) {
    return false;
  }
  if (spelling[integer_begin] == '0' && index != integer_begin + 1U) {
    return false;
  }
  bool has_fraction = false;
  bool has_exponent = false;
  if (index < spelling.size() && spelling[index] == '.') {
    has_fraction = true;
    ++index;
    const std::size_t fraction_begin = index;
    while (index < spelling.size() && is_digit(spelling[index])) {
      ++index;
    }
    if (fraction_begin == index) {
      return false;
    }
  }
  if (index < spelling.size() &&
      (spelling[index] == 'e' || spelling[index] == 'E')) {
    has_exponent = true;
    ++index;
    if (index < spelling.size() &&
        (spelling[index] == '+' || spelling[index] == '-')) {
      ++index;
    }
    const std::size_t exponent_begin = index;
    while (index < spelling.size() && is_digit(spelling[index])) {
      ++index;
    }
    if (exponent_begin == index) {
      return false;
    }
  }
  return index == spelling.size() && (has_fraction || has_exponent);
}

std::int64_t decimal_scientific_exponent(std::string_view spelling) {
  std::size_t index = spelling.front() == '-' ? 1U : 0U;
  std::size_t digits_before_decimal = 0U;
  std::size_t digit_ordinal = 0U;
  const std::size_t missing = std::numeric_limits<std::size_t>::max();
  std::size_t first_nonzero = missing;
  while (index < spelling.size() && spelling[index] != 'e' &&
         spelling[index] != 'E') {
    if (spelling[index] == '.') {
      digits_before_decimal = digit_ordinal;
    } else {
      if (first_nonzero == missing && spelling[index] != '0') {
        first_nonzero = digit_ordinal;
      }
      ++digit_ordinal;
    }
    ++index;
  }
  if (spelling.find('.') == std::string_view::npos) {
    digits_before_decimal = digit_ordinal;
  }
  if (first_nonzero == missing) {
    return 0;
  }

  std::int64_t explicit_exponent = 0;
  if (index < spelling.size()) {
    ++index;
    bool negative = false;
    if (index < spelling.size() &&
        (spelling[index] == '+' || spelling[index] == '-')) {
      negative = spelling[index] == '-';
      ++index;
    }
    while (index < spelling.size()) {
      if (explicit_exponent < 1000000) {
        explicit_exponent =
            explicit_exponent * 10 + (spelling[index] - '0');
      }
      ++index;
    }
    if (negative) {
      explicit_exponent = -explicit_exponent;
    }
  }
  const std::int64_t before =
      digits_before_decimal > 1000000U
          ? 1000000
          : static_cast<std::int64_t>(digits_before_decimal);
  const std::int64_t leading =
      first_nonzero > 1000000U
          ? 1000000
          : static_cast<std::int64_t>(first_nonzero);
  return explicit_exponent + before - leading - 1;
}

bool decimal_rounds_to_zero(std::string_view spelling) {
  const std::int64_t exponent = decimal_scientific_exponent(spelling);
  if (exponent < -324) {
    return true;
  }
  if (exponent != -324) {
    return false;
  }
  constexpr std::string_view half_minimum_subnormal =
      "24703282292062327208828439643411068618252990130716238221279284125033775";
  std::size_t index = spelling.front() == '-' ? 1U : 0U;
  std::size_t significant_index = 0U;
  bool found_nonzero = false;
  while (index < spelling.size() && spelling[index] != 'e' &&
         spelling[index] != 'E') {
    if (spelling[index] != '.') {
      if (spelling[index] != '0' || found_nonzero) {
        found_nonzero = true;
        const char threshold =
            significant_index < half_minimum_subnormal.size()
                ? half_minimum_subnormal[significant_index]
                : '0';
        if (spelling[index] < threshold) {
          return true;
        }
        if (spelling[index] > threshold) {
          return false;
        }
        ++significant_index;
      }
    }
    ++index;
  }
  while (significant_index < half_minimum_subnormal.size()) {
    if (half_minimum_subnormal[significant_index] != '0') {
      return true;
    }
    ++significant_index;
  }
  return true;
}

struct RewriteDoubleParser {
#if defined(__APPLE__) || defined(BENNU_REWRITE_STRTOD_L)
  locale_t c_locale;
#endif
};

RewriteDoubleParser make_rewrite_double_parser() {
#if defined(__APPLE__) || defined(BENNU_REWRITE_STRTOD_L)
  return RewriteDoubleParser{newlocale(LC_NUMERIC_MASK, "C", nullptr)};
#else
  return RewriteDoubleParser{};
#endif
}

void destroy_rewrite_double_parser(RewriteDoubleParser parser) {
#if defined(__APPLE__) || defined(BENNU_REWRITE_STRTOD_L)
  if (parser.c_locale != nullptr) {
    freelocale(parser.c_locale);
  }
#else
  static_cast<void>(parser);
#endif
}

std::from_chars_result parse_finite_double(
    std::string_view spelling, const RewriteDoubleParser &parser,
    double &converted_value) {
#if defined(__APPLE__) || defined(BENNU_REWRITE_STRTOD_L)
  if (parser.c_locale == nullptr) {
    return std::from_chars_result{spelling.data(),
                                  std::errc::invalid_argument};
  }
  const int previous_rounding = std::fegetround();
  if (previous_rounding == -1 ||
      (previous_rounding != FE_TONEAREST &&
       std::fesetround(FE_TONEAREST) != 0)) {
    return std::from_chars_result{spelling.data(),
                                  std::errc::invalid_argument};
  }

  const std::string terminated(spelling);
  char *converted_end = nullptr;
  errno = 0;
  converted_value =
      strtod_l(terminated.c_str(), &converted_end, parser.c_locale);
  const int conversion_errno = errno;
  const bool restore_failed =
      previous_rounding != FE_TONEAREST &&
      std::fesetround(previous_rounding) != 0;
  if (restore_failed) {
    return std::from_chars_result{spelling.data(),
                                  std::errc::invalid_argument};
  }

  const std::size_t converted_size =
      static_cast<std::size_t>(converted_end - terminated.c_str());
  std::errc error{};
  if (conversion_errno == ERANGE &&
      (converted_value == 0.0 || !std::isfinite(converted_value))) {
    error = std::errc::result_out_of_range;
  } else if (converted_size != spelling.size()) {
    error = std::errc::invalid_argument;
  }
  return std::from_chars_result{spelling.data() + converted_size, error};
#else
  static_cast<void>(parser);
  return std::from_chars(spelling.data(), spelling.data() + spelling.size(),
                         converted_value, std::chars_format::general);
#endif
}

RewriteToken numeric_token(std::string_view spelling, RewritePosition begin,
                           RewritePosition end,
                           const RewriteDoubleParser &double_parser) {
  RewriteToken token =
      make_token(RewriteTokenKind::malformed_literal, begin, end);
  token.literal_error = RewriteLiteralError::malformed;
  if (spelling == "-inf") {
    token.kind = RewriteTokenKind::double_literal;
    token.double_precision = -std::numeric_limits<double>::infinity();
    token.literal_error = RewriteLiteralError::none;
    return token;
  }

  std::int64_t integer = 0;
  if (parse_canonical_integer(spelling, integer)) {
    token.kind = RewriteTokenKind::int_literal;
    token.integer = integer;
    token.literal_error = RewriteLiteralError::none;
    return token;
  }
  if (canonical_integer_grammar(spelling)) {
    token.literal_error = RewriteLiteralError::range;
    return token;
  }

  if (!finite_double_grammar(spelling)) {
    return token;
  }
  double converted_value = 0.0;
  const auto converted =
      parse_finite_double(spelling, double_parser, converted_value);
  if (converted.ec == std::errc::result_out_of_range) {
    if (decimal_rounds_to_zero(spelling)) {
      token.kind = RewriteTokenKind::double_literal;
      token.double_precision = spelling.front() == '-'
                                   ? -0.0
                                   : 0.0;
      token.literal_error = RewriteLiteralError::none;
      return token;
    }
    token.literal_error = RewriteLiteralError::range;
    return token;
  }
  if (converted.ec != std::errc{} ||
      converted.ptr != spelling.data() + spelling.size() ||
      !std::isfinite(converted_value)) {
    token.literal_error = RewriteLiteralError::range;
    return token;
  }
  token.kind = RewriteTokenKind::double_literal;
  token.double_precision = converted_value;
  token.literal_error = RewriteLiteralError::none;
  return token;
}

RewriteTokens tokenize_rewrite(std::string_view source) {
  RewriteTokens result{};
  result.source.assign(source);
  const RewriteDoubleParser double_parser = make_rewrite_double_parser();
  std::size_t index = 0U;
  RewritePosition position{1U, 1U, 1U};
  while (index < source.size()) {
    const RewritePosition begin = position;
    const char byte = source[index];
    if (is_horizontal_space(byte)) {
      do {
        ++index;
        ++position.offset;
        ++position.column;
      } while (index < source.size() && is_horizontal_space(source[index]));
      result.tokens.push_back(make_token(
          RewriteTokenKind::horizontal_space, begin, position));
      continue;
    }
    if (byte == '\n' ||
        (byte == '\r' && index + 1U < source.size() &&
         source[index + 1U] == '\n')) {
      if (byte == '\r') {
        index += 2U;
        position.offset += 2U;
      } else {
        ++index;
        ++position.offset;
      }
      ++position.line;
      position.column = 1U;
      result.tokens.push_back(make_token(
          RewriteTokenKind::line_terminator, begin, position));
      continue;
    }
    if (is_lowercase(byte)) {
      do {
        ++index;
        ++position.offset;
        ++position.column;
      } while (index < source.size() &&
               (is_lowercase(source[index]) || is_digit(source[index]) ||
                source[index] == '_'));
      const std::string_view spelling = source.substr(
          begin.offset - 1U, position.offset - begin.offset);
      RewriteToken token = make_token(RewriteTokenKind::name, begin, position);
      if (spelling == "true" || spelling == "false") {
        token.kind = RewriteTokenKind::bool_literal;
        token.boolean = spelling == "true";
      } else if (spelling == "inf" || spelling == "nan") {
        token.kind = RewriteTokenKind::double_literal;
        token.double_precision = spelling == "inf"
                                     ? std::numeric_limits<double>::infinity()
                                     : std::numeric_limits<double>::quiet_NaN();
      }
      result.tokens.push_back(token);
      continue;
    }
    if (is_uppercase(byte)) {
      do {
        ++index;
        ++position.offset;
        ++position.column;
      } while (index < source.size() &&
               (is_uppercase(source[index]) ||
                is_lowercase(source[index]) || is_digit(source[index]) ||
                source[index] == '_'));
      const std::string_view spelling = source.substr(
          begin.offset - 1U, position.offset - begin.offset);
      RewriteTokenKind kind = RewriteTokenKind::invalid;
      if (spelling == "Bool") {
        kind = RewriteTokenKind::bool_type;
      } else if (spelling == "Int") {
        kind = RewriteTokenKind::int_type;
      } else if (spelling == "Double") {
        kind = RewriteTokenKind::double_type;
      }
      result.tokens.push_back(make_token(kind, begin, position));
      continue;
    }
    if (is_digit(byte) || byte == '-' || byte == '+' || byte == '.') {
      do {
        ++index;
        ++position.offset;
        ++position.column;
      } while (index < source.size() &&
               is_numeric_candidate_byte(source[index]));
      const std::string_view spelling = source.substr(
          begin.offset - 1U, position.offset - begin.offset);
      result.tokens.push_back(
          numeric_token(spelling, begin, position, double_parser));
      continue;
    }

    RewriteTokenKind kind = RewriteTokenKind::invalid;
    switch (byte) {
    case '[':
      kind = RewriteTokenKind::left_bracket;
      break;
    case ']':
      kind = RewriteTokenKind::right_bracket;
      break;
    case '(':
      kind = RewriteTokenKind::left_parenthesis;
      break;
    case ')':
      kind = RewriteTokenKind::right_parenthesis;
      break;
    default:
      break;
    }
    ++index;
    ++position.offset;
    ++position.column;
    result.tokens.push_back(make_token(kind, begin, position));
  }
  result.end = position;
  destroy_rewrite_double_parser(double_parser);
  return result;
}

enum class RewriteNodeKind {
  scalar_literal,
  vector_literal,
  parameter_reference,
  primitive_call,
};

enum class RewriteCallSyntax {
  bracketed,
  prefix,
};

enum class RewriteParseError {
  none,
  invalid_byte,
  malformed_literal,
  literal_range,
  expected_expression,
  primitive_requires_application,
  whitespace_before_bracket,
  missing_separator,
  mismatched_delimiter,
  missing_delimiter,
  bare_empty_vector,
  heterogeneous_vector,
  invalid_vector_element,
  trailing_input,
  unknown_primitive,
};

struct RewriteNode {
  RewriteNodeKind kind;
  RewriteSpan span;
  ScalarType element_type;
  bool boolean;
  std::int64_t integer;
  double double_precision;
  std::size_t first_element;
  std::size_t element_count;
  std::size_t first_element_span;
  std::size_t call_index;
};

struct RewriteCall {
  RewriteCallSyntax syntax;
  RewriteSpan name_span;
  RewriteSpan opening_delimiter_span;
  RewriteSpan closing_delimiter_span;
  RewriteSpan prefix_separator_span;
  RewriteSpan span;
  std::size_t first_argument;
  std::size_t argument_count;
  std::optional<PrimitiveId> primitive;
};

struct RewriteDiagnostic {
  RewriteParseError error;
  RewriteSpan primary;
  RewriteSpan context;
  RewriteSpan related;
};

struct RewriteProgram {
  std::string source;
  std::vector<RewriteNode> nodes;
  std::vector<std::size_t> arguments;
  std::vector<RewriteSpan> argument_spans;
  std::vector<std::size_t> roots;
  std::vector<RewriteCall> calls;
  std::vector<std::uint8_t> boolean_elements;
  std::vector<std::int64_t> integer_elements;
  std::vector<double> double_elements;
  std::vector<RewriteSpan> vector_element_spans;
};

struct RewriteParseResult {
  bool ok;
  RewriteProgram program;
  RewriteDiagnostic diagnostic;
};

enum class RewriteContextKind {
  bracket_call,
  prefix_call,
};

struct RewritePendingArgument {
  std::size_t node;
  std::size_t next;
};

struct RewriteContext {
  RewriteContextKind kind;
  RewriteSpan name_span;
  RewriteSpan opening_span;
  RewriteSpan separator_span;
  std::size_t first_pending_argument;
  std::size_t last_pending_argument;
  std::size_t argument_count;
  std::size_t opening_token_index;
  bool after_argument;
};

constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();

RewriteSpan insertion_span(RewritePosition position) {
  return RewriteSpan{position, position};
}

ScalarType scalar_token_type(RewriteTokenKind kind) {
  if (kind == RewriteTokenKind::bool_literal) {
    return ScalarType::boolean;
  }
  if (kind == RewriteTokenKind::int_literal) {
    return ScalarType::integer;
  }
  return ScalarType::double_precision;
}

bool is_scalar_token(RewriteTokenKind kind) {
  return kind == RewriteTokenKind::bool_literal ||
         kind == RewriteTokenKind::int_literal ||
         kind == RewriteTokenKind::double_literal;
}

RewriteNode scalar_node(const RewriteToken &token) {
  return RewriteNode{RewriteNodeKind::scalar_literal,
                     token.span,
                     scalar_token_type(token.kind),
                     token.boolean,
                     token.integer,
                     token.double_precision,
                     0U,
                     0U,
                     0U,
                     0U};
}

void set_diagnostic(RewriteParseResult &result, RewriteParseError error,
                    RewriteSpan primary, RewriteSpan context,
                    RewriteSpan related) {
  result.ok = false;
  result.diagnostic = RewriteDiagnostic{error, primary, context, related};
}

RewriteSpan delimited_context_span(const RewriteTokens &tokens,
                                   std::size_t opening_index,
                                   RewriteTokenKind closing_kind) {
  const RewriteTokenKind opening_kind = tokens.tokens[opening_index].kind;
  std::size_t depth = 0U;
  for (std::size_t index = opening_index; index < tokens.tokens.size(); ++index) {
    if (tokens.tokens[index].kind == opening_kind) {
      ++depth;
    } else if (tokens.tokens[index].kind == closing_kind) {
      if (depth == 1U) {
        return RewriteSpan{tokens.tokens[opening_index].span.begin,
                           tokens.tokens[index].span.end};
      }
      if (depth > 1U) {
        --depth;
      }
    }
  }
  return RewriteSpan{tokens.tokens[opening_index].span.begin, tokens.end};
}

RewriteSpan bracket_call_context_span(const RewriteTokens &tokens,
                                      const RewriteContext &context) {
  std::size_t depth = 0U;
  for (std::size_t index = context.opening_token_index;
       index < tokens.tokens.size(); ++index) {
    if (tokens.tokens[index].kind == RewriteTokenKind::left_bracket) {
      ++depth;
    } else if (tokens.tokens[index].kind == RewriteTokenKind::right_bracket) {
      if (depth == 1U) {
        return RewriteSpan{context.name_span.begin,
                           tokens.tokens[index].span.end};
      }
      if (depth > 1U) {
        --depth;
      }
    }
  }
  return RewriteSpan{context.name_span.begin, tokens.end};
}

bool append_vector_element(RewriteProgram &program, const RewriteToken &token,
                           ScalarType element_type) {
  program.vector_element_spans.push_back(token.span);
  if (element_type == ScalarType::boolean) {
    program.boolean_elements.push_back(token.boolean ? std::uint8_t{1U}
                                                     : std::uint8_t{0U});
  } else if (element_type == ScalarType::integer) {
    program.integer_elements.push_back(token.integer);
  } else {
    program.double_elements.push_back(token.double_precision);
  }
  return true;
}

std::size_t vector_payload_size(const RewriteProgram &program,
                                ScalarType element_type) {
  if (element_type == ScalarType::boolean) {
    return program.boolean_elements.size();
  }
  if (element_type == ScalarType::integer) {
    return program.integer_elements.size();
  }
  return program.double_elements.size();
}

bool parse_vector_literal(const RewriteTokens &tokens, std::size_t &token_index,
                          RewriteParseResult &result,
                          std::size_t &completed_node) {
  const std::size_t opening_index = token_index;
  const RewriteToken &opening = tokens.tokens[token_index];
  ++token_index;
  while (token_index < tokens.tokens.size() &&
         (tokens.tokens[token_index].kind ==
              RewriteTokenKind::horizontal_space ||
          tokens.tokens[token_index].kind ==
              RewriteTokenKind::line_terminator)) {
    ++token_index;
  }
  if (token_index == tokens.tokens.size()) {
    const RewriteSpan insertion = insertion_span(tokens.end);
    set_diagnostic(result, RewriteParseError::missing_delimiter, insertion,
                   RewriteSpan{opening.span.begin, tokens.end}, opening.span);
    return false;
  }
  if (tokens.tokens[token_index].kind ==
      RewriteTokenKind::right_parenthesis) {
    const RewriteSpan complete{opening.span.begin,
                               tokens.tokens[token_index].span.end};
    set_diagnostic(result, RewriteParseError::bare_empty_vector, complete,
                   complete, opening.span);
    return false;
  }
  if (tokens.tokens[token_index].kind == RewriteTokenKind::right_bracket) {
    set_diagnostic(result, RewriteParseError::mismatched_delimiter,
                   tokens.tokens[token_index].span,
                   RewriteSpan{opening.span.begin,
                               tokens.tokens[token_index].span.end},
                   opening.span);
    return false;
  }
  if (!is_scalar_token(tokens.tokens[token_index].kind)) {
    const RewriteToken &invalid = tokens.tokens[token_index];
    RewriteParseError error = RewriteParseError::invalid_vector_element;
    if (invalid.kind == RewriteTokenKind::malformed_literal) {
      error = invalid.literal_error == RewriteLiteralError::range
                  ? RewriteParseError::literal_range
                  : RewriteParseError::malformed_literal;
    } else if (invalid.kind == RewriteTokenKind::invalid) {
      error = RewriteParseError::invalid_byte;
    }
    set_diagnostic(result, error, invalid.span,
                   delimited_context_span(
                       tokens, opening_index,
                       RewriteTokenKind::right_parenthesis),
                   opening.span);
    return false;
  }

  const ScalarType element_type =
      scalar_token_type(tokens.tokens[token_index].kind);
  const std::size_t first_element =
      vector_payload_size(result.program, element_type);
  const std::size_t first_element_span =
      result.program.vector_element_spans.size();
  std::size_t element_count = 0U;
  while (true) {
    const RewriteToken &element = tokens.tokens[token_index];
    if (!is_scalar_token(element.kind)) {
      RewriteParseError error = RewriteParseError::invalid_vector_element;
      if (element.kind == RewriteTokenKind::malformed_literal) {
        error = element.literal_error == RewriteLiteralError::range
                    ? RewriteParseError::literal_range
                    : RewriteParseError::malformed_literal;
      } else if (element.kind == RewriteTokenKind::invalid) {
        error = RewriteParseError::invalid_byte;
      }
      set_diagnostic(result, error, element.span,
                     delimited_context_span(
                         tokens, opening_index,
                         RewriteTokenKind::right_parenthesis),
                     opening.span);
      return false;
    }
    if (scalar_token_type(element.kind) != element_type) {
      set_diagnostic(result, RewriteParseError::heterogeneous_vector,
                     element.span,
                     delimited_context_span(
                         tokens, opening_index,
                         RewriteTokenKind::right_parenthesis),
                     opening.span);
      return false;
    }
    append_vector_element(result.program, element, element_type);
    ++element_count;
    ++token_index;
    if (token_index == tokens.tokens.size()) {
      const RewriteSpan insertion = insertion_span(tokens.end);
      set_diagnostic(result, RewriteParseError::missing_delimiter, insertion,
                     RewriteSpan{opening.span.begin, tokens.end}, opening.span);
      return false;
    }
    if (tokens.tokens[token_index].kind ==
        RewriteTokenKind::right_parenthesis) {
      const RewriteSpan complete{opening.span.begin,
                                 tokens.tokens[token_index].span.end};
      ++token_index;
      result.program.nodes.push_back(
          RewriteNode{RewriteNodeKind::vector_literal,
                      complete,
                      element_type,
                      false,
                      0,
                      0.0,
                      first_element,
                      element_count,
                      first_element_span,
                      0U});
      completed_node = result.program.nodes.size() - 1U;
      return true;
    }
    if (tokens.tokens[token_index].kind == RewriteTokenKind::right_bracket) {
      set_diagnostic(result, RewriteParseError::mismatched_delimiter,
                     tokens.tokens[token_index].span,
                     RewriteSpan{opening.span.begin,
                                 tokens.tokens[token_index].span.end},
                     opening.span);
      return false;
    }
    if (tokens.tokens[token_index].kind !=
            RewriteTokenKind::horizontal_space &&
        tokens.tokens[token_index].kind !=
            RewriteTokenKind::line_terminator) {
      const RewriteParseError error =
          tokens.tokens[token_index].kind == RewriteTokenKind::invalid
              ? RewriteParseError::invalid_byte
              : RewriteParseError::missing_separator;
      set_diagnostic(result, error,
                     tokens.tokens[token_index].span,
                     delimited_context_span(
                         tokens, opening_index,
                         RewriteTokenKind::right_parenthesis),
                     opening.span);
      return false;
    }
    while (token_index < tokens.tokens.size() &&
           (tokens.tokens[token_index].kind ==
                RewriteTokenKind::horizontal_space ||
            tokens.tokens[token_index].kind ==
                RewriteTokenKind::line_terminator)) {
      ++token_index;
    }
    if (token_index == tokens.tokens.size()) {
      const RewriteSpan insertion = insertion_span(tokens.end);
      set_diagnostic(result, RewriteParseError::missing_delimiter, insertion,
                     RewriteSpan{opening.span.begin, tokens.end}, opening.span);
      return false;
    }
    if (tokens.tokens[token_index].kind ==
        RewriteTokenKind::right_parenthesis) {
      const RewriteSpan complete{opening.span.begin,
                                 tokens.tokens[token_index].span.end};
      ++token_index;
      result.program.nodes.push_back(
          RewriteNode{RewriteNodeKind::vector_literal,
                      complete,
                      element_type,
                      false,
                      0,
                      0.0,
                      first_element,
                      element_count,
                      first_element_span,
                      0U});
      completed_node = result.program.nodes.size() - 1U;
      return true;
    }
    if (tokens.tokens[token_index].kind == RewriteTokenKind::right_bracket) {
      set_diagnostic(result, RewriteParseError::mismatched_delimiter,
                     tokens.tokens[token_index].span,
                     RewriteSpan{opening.span.begin,
                                 tokens.tokens[token_index].span.end},
                     opening.span);
      return false;
    }
  }
}

void append_pending_argument(std::vector<RewritePendingArgument> &pending,
                             RewriteContext &context, std::size_t node) {
  const std::size_t pending_index = pending.size();
  pending.push_back(RewritePendingArgument{node, no_index});
  if (context.first_pending_argument == no_index) {
    context.first_pending_argument = pending_index;
  } else {
    pending[context.last_pending_argument].next = pending_index;
  }
  context.last_pending_argument = pending_index;
  ++context.argument_count;
}

std::size_t finish_call(RewriteProgram &program,
                        const std::vector<RewritePendingArgument> &pending,
                        const RewriteContext &context,
                        RewriteSpan closing_span, RewriteSpan complete_span) {
  const std::size_t first_argument = program.arguments.size();
  std::size_t pending_index = context.first_pending_argument;
  while (pending_index != no_index) {
    const std::size_t node_index = pending[pending_index].node;
    program.arguments.push_back(node_index);
    program.argument_spans.push_back(program.nodes[node_index].span);
    pending_index = pending[pending_index].next;
  }
  const std::size_t call_index = program.calls.size();
  program.calls.push_back(RewriteCall{
      context.kind == RewriteContextKind::bracket_call
          ? RewriteCallSyntax::bracketed
          : RewriteCallSyntax::prefix,
      context.name_span,
      context.opening_span,
      closing_span,
      context.separator_span,
      complete_span,
      first_argument,
      context.argument_count,
      std::nullopt});
  program.nodes.push_back(RewriteNode{RewriteNodeKind::primitive_call,
                                     complete_span,
                                     ScalarType::boolean,
                                     false,
                                     0,
                                     0.0,
                                     0U,
                                     0U,
                                     0U,
                                     call_index});
  return program.nodes.size() - 1U;
}

bool token_starts_expression(RewriteTokenKind kind) {
  return is_scalar_token(kind) || kind == RewriteTokenKind::name ||
         kind == RewriteTokenKind::left_parenthesis ||
         kind == RewriteTokenKind::bool_type ||
         kind == RewriteTokenKind::int_type ||
         kind == RewriteTokenKind::double_type ||
         kind == RewriteTokenKind::malformed_literal ||
         kind == RewriteTokenKind::invalid;
}

RewriteParseResult parse_rewrite(std::string_view source) {
  RewriteParseResult result{};
  RewriteTokens tokens = tokenize_rewrite(source);
  result.program.source = tokens.source;
  result.diagnostic = RewriteDiagnostic{RewriteParseError::none,
                                        insertion_span(tokens.end),
                                        insertion_span(tokens.end),
                                        insertion_span(tokens.end)};
  std::vector<RewriteContext> contexts;
  std::vector<RewritePendingArgument> pending_arguments;
  std::size_t token_index = 0U;
  std::size_t completed_node = 0U;
  bool have_expression = false;

  while (true) {
    if (have_expression) {
      while (!contexts.empty() &&
             contexts.back().kind == RewriteContextKind::prefix_call) {
        const RewriteContext context = contexts.back();
        contexts.pop_back();
        RewriteContext completed_context = context;
        append_pending_argument(pending_arguments, completed_context,
                                completed_node);
        const RewriteSpan complete{context.name_span.begin,
                                   result.program.nodes[completed_node].span.end};
        completed_node = finish_call(
            result.program, pending_arguments, completed_context,
            insertion_span(context.separator_span.end), complete);
      }
      if (!contexts.empty()) {
        RewriteContext &context = contexts.back();
        append_pending_argument(pending_arguments, context, completed_node);
        context.after_argument = true;
        have_expression = false;
        continue;
      }

      result.program.roots.push_back(completed_node);
      have_expression = false;
      while (token_index < tokens.tokens.size() &&
             tokens.tokens[token_index].kind ==
                 RewriteTokenKind::horizontal_space) {
        ++token_index;
      }
      if (token_index == tokens.tokens.size()) {
        result.ok = true;
        return result;
      }
      if (tokens.tokens[token_index].kind ==
          RewriteTokenKind::line_terminator) {
        ++token_index;
        continue;
      }
      RewriteParseError error = RewriteParseError::trailing_input;
      if (tokens.tokens[token_index].kind == RewriteTokenKind::invalid) {
        error = RewriteParseError::invalid_byte;
      } else if (tokens.tokens[token_index].kind ==
                 RewriteTokenKind::malformed_literal) {
        error = tokens.tokens[token_index].literal_error ==
                        RewriteLiteralError::range
                    ? RewriteParseError::literal_range
                    : RewriteParseError::malformed_literal;
      } else if (tokens.tokens[token_index].kind ==
                     RewriteTokenKind::right_bracket ||
                 tokens.tokens[token_index].kind ==
                     RewriteTokenKind::right_parenthesis) {
        error = RewriteParseError::mismatched_delimiter;
      }
      set_diagnostic(result, error,
                     tokens.tokens[token_index].span,
                     result.program.nodes[completed_node].span,
                     result.program.nodes[completed_node].span);
      return result;
    }

    if (!contexts.empty() &&
        contexts.back().kind == RewriteContextKind::bracket_call) {
      RewriteContext &context = contexts.back();
      if (context.after_argument) {
        if (token_index < tokens.tokens.size() &&
            tokens.tokens[token_index].kind ==
                RewriteTokenKind::right_bracket) {
          const RewriteSpan closing = tokens.tokens[token_index].span;
          ++token_index;
          const RewriteContext completed_context = context;
          contexts.pop_back();
          completed_node = finish_call(
              result.program, pending_arguments, completed_context, closing,
              RewriteSpan{completed_context.name_span.begin, closing.end});
          have_expression = true;
          continue;
        }
        if (token_index < tokens.tokens.size() &&
            (tokens.tokens[token_index].kind ==
                 RewriteTokenKind::horizontal_space ||
             tokens.tokens[token_index].kind ==
                 RewriteTokenKind::line_terminator)) {
          while (token_index < tokens.tokens.size() &&
                 (tokens.tokens[token_index].kind ==
                      RewriteTokenKind::horizontal_space ||
                  tokens.tokens[token_index].kind ==
                      RewriteTokenKind::line_terminator)) {
            ++token_index;
          }
          if (token_index < tokens.tokens.size() &&
              tokens.tokens[token_index].kind ==
                  RewriteTokenKind::right_bracket) {
            continue;
          }
          context.after_argument = false;
        } else if (token_index == tokens.tokens.size()) {
          const RewriteSpan insertion = insertion_span(tokens.end);
          set_diagnostic(
              result, RewriteParseError::missing_delimiter, insertion,
              bracket_call_context_span(tokens, context),
              context.opening_span);
          return result;
        } else if (tokens.tokens[token_index].kind ==
                   RewriteTokenKind::right_parenthesis) {
          set_diagnostic(
              result, RewriteParseError::mismatched_delimiter,
              tokens.tokens[token_index].span,
              bracket_call_context_span(tokens, context),
              context.opening_span);
          return result;
        } else {
          const RewriteParseError error =
              tokens.tokens[token_index].kind == RewriteTokenKind::invalid
                  ? RewriteParseError::invalid_byte
                  : RewriteParseError::missing_separator;
          set_diagnostic(
              result, error,
              tokens.tokens[token_index].span,
              bracket_call_context_span(tokens, context),
              context.opening_span);
          return result;
        }
      }
      if (token_index == tokens.tokens.size()) {
        const RewriteSpan insertion = insertion_span(tokens.end);
        set_diagnostic(result, RewriteParseError::missing_delimiter, insertion,
                       bracket_call_context_span(tokens, context),
                       context.opening_span);
        return result;
      }
      if (tokens.tokens[token_index].kind ==
          RewriteTokenKind::right_bracket) {
        const RewriteSpan closing = tokens.tokens[token_index].span;
        ++token_index;
        const RewriteContext completed_context = context;
        contexts.pop_back();
        completed_node = finish_call(
            result.program, pending_arguments, completed_context, closing,
            RewriteSpan{completed_context.name_span.begin, closing.end});
        have_expression = true;
        continue;
      }
      if (tokens.tokens[token_index].kind ==
          RewriteTokenKind::right_parenthesis) {
        set_diagnostic(
            result, RewriteParseError::mismatched_delimiter,
            tokens.tokens[token_index].span,
            bracket_call_context_span(tokens, context),
            context.opening_span);
        return result;
      }
    } else if (contexts.empty()) {
      while (token_index < tokens.tokens.size() &&
             (tokens.tokens[token_index].kind ==
                  RewriteTokenKind::horizontal_space ||
              tokens.tokens[token_index].kind ==
                  RewriteTokenKind::line_terminator)) {
        ++token_index;
      }
      if (token_index == tokens.tokens.size()) {
        result.ok = true;
        return result;
      }
    }

    const RewriteToken &token = tokens.tokens[token_index];
    if (is_scalar_token(token.kind)) {
      result.program.nodes.push_back(scalar_node(token));
      completed_node = result.program.nodes.size() - 1U;
      ++token_index;
      have_expression = true;
      continue;
    }
    if (token.kind == RewriteTokenKind::left_parenthesis) {
      if (!parse_vector_literal(tokens, token_index, result,
                                completed_node)) {
        return result;
      }
      have_expression = true;
      continue;
    }
    if (token.kind == RewriteTokenKind::bool_type ||
        token.kind == RewriteTokenKind::int_type ||
        token.kind == RewriteTokenKind::double_type) {
      if (token_index + 2U < tokens.tokens.size() &&
          tokens.tokens[token_index + 1U].kind ==
              RewriteTokenKind::left_parenthesis &&
          tokens.tokens[token_index + 2U].kind ==
              RewriteTokenKind::right_parenthesis) {
        const ScalarType type = token.kind == RewriteTokenKind::bool_type
                                    ? ScalarType::boolean
                                : token.kind == RewriteTokenKind::int_type
                                    ? ScalarType::integer
                                    : ScalarType::double_precision;
        const RewriteSpan complete{token.span.begin,
                                   tokens.tokens[token_index + 2U].span.end};
        result.program.nodes.push_back(
            RewriteNode{RewriteNodeKind::vector_literal,
                        complete,
                        type,
                        false,
                        0,
                        0.0,
                        vector_payload_size(result.program, type),
                        0U,
                        result.program.vector_element_spans.size(),
                        0U});
        completed_node = result.program.nodes.size() - 1U;
        token_index += 3U;
        have_expression = true;
        continue;
      }
      set_diagnostic(result, RewriteParseError::invalid_vector_element,
                     token.span, token.span, token.span);
      return result;
    }
    if (token.kind == RewriteTokenKind::name) {
      if (token_index + 1U < tokens.tokens.size() &&
          tokens.tokens[token_index + 1U].kind ==
              RewriteTokenKind::left_bracket) {
        const RewriteSpan opening = tokens.tokens[token_index + 1U].span;
        contexts.push_back(RewriteContext{
            RewriteContextKind::bracket_call,
            token.span,
            opening,
            insertion_span(opening.end),
            no_index,
            no_index,
            0U,
            token_index + 1U,
            false});
        token_index += 2U;
        while (token_index < tokens.tokens.size() &&
               (tokens.tokens[token_index].kind ==
                    RewriteTokenKind::horizontal_space ||
                tokens.tokens[token_index].kind ==
                    RewriteTokenKind::line_terminator)) {
          ++token_index;
        }
        continue;
      }
      if (token_index + 1U < tokens.tokens.size() &&
          tokens.tokens[token_index + 1U].kind ==
              RewriteTokenKind::horizontal_space) {
        if (token_index + 2U < tokens.tokens.size() &&
            tokens.tokens[token_index + 2U].kind ==
                RewriteTokenKind::left_bracket) {
          set_diagnostic(result, RewriteParseError::whitespace_before_bracket,
                         tokens.tokens[token_index + 1U].span, token.span,
                         tokens.tokens[token_index + 2U].span);
          return result;
        }
        const RewriteSpan separator = tokens.tokens[token_index + 1U].span;
        contexts.push_back(RewriteContext{RewriteContextKind::prefix_call,
                                          token.span,
                                          insertion_span(separator.begin),
                                          separator,
                                          no_index,
                                          no_index,
                                          0U,
                                          no_index,
                                          false});
        token_index += 2U;
        if (token_index == tokens.tokens.size() ||
            tokens.tokens[token_index].kind ==
                RewriteTokenKind::line_terminator) {
          const RewriteSpan primary =
              token_index == tokens.tokens.size()
                  ? insertion_span(tokens.end)
                  : tokens.tokens[token_index].span;
          set_diagnostic(result, RewriteParseError::expected_expression,
                         primary,
                         RewriteSpan{token.span.begin, primary.end},
                         separator);
          return result;
        }
        continue;
      }
      set_diagnostic(result,
                     RewriteParseError::primitive_requires_application,
                     token.span, token.span, token.span);
      return result;
    }
    if (token.kind == RewriteTokenKind::malformed_literal) {
      set_diagnostic(
          result,
          token.literal_error == RewriteLiteralError::range
              ? RewriteParseError::literal_range
              : RewriteParseError::malformed_literal,
          token.span, token.span, token.span);
      return result;
    }
    if (token.kind == RewriteTokenKind::invalid) {
      set_diagnostic(result, RewriteParseError::invalid_byte, token.span,
                     token.span, token.span);
      return result;
    }
    if (token.kind == RewriteTokenKind::right_bracket ||
        token.kind == RewriteTokenKind::right_parenthesis) {
      set_diagnostic(result, RewriteParseError::mismatched_delimiter,
                     token.span, token.span, token.span);
      return result;
    }
    const RewriteParseError error =
        token_starts_expression(token.kind)
            ? RewriteParseError::expected_expression
            : RewriteParseError::invalid_byte;
    set_diagnostic(result, error, token.span, token.span, token.span);
    return result;
  }
}

struct RewriteResolutionResult {
  bool ok;
  RewriteDiagnostic diagnostic;
};

RewriteResolutionResult resolve_rewrite_primitives(RewriteProgram &program) {
  std::vector<PrimitiveId> resolved_ids;
  resolved_ids.reserve(program.calls.size());
  for (const RewriteCall &call : program.calls) {
    const std::size_t name_begin = call.name_span.begin.offset - 1U;
    const std::size_t name_size =
        call.name_span.end.offset - call.name_span.begin.offset;
    const std::string_view name(program.source.data() + name_begin, name_size);
    const PrimitiveDescriptor *descriptor = find_primitive(name);
    if (descriptor == nullptr) {
      return RewriteResolutionResult{
          false,
          RewriteDiagnostic{RewriteParseError::unknown_primitive,
                            call.name_span, call.span, call.name_span}};
    }
    resolved_ids.push_back(descriptor->id);
  }
  for (std::size_t index = 0U; index < program.calls.size(); ++index) {
    program.calls[index].primitive = resolved_ids[index];
  }
  const RewriteSpan empty = insertion_span(RewritePosition{1U, 1U, 1U});
  return RewriteResolutionResult{
      true,
      RewriteDiagnostic{RewriteParseError::none, empty, empty, empty}};
}

enum class RewriteEvaluationStage {
  none,
  parse,
  resolution,
  primitive_table,
  resource_admission,
  literal,
  application,
  formatting,
};

struct RewriteEvaluationCreationData {
  ExecutionProfile profile;
  ResourceLimits limits;
  AllocationFailureInjection allocation_failure;
};

struct CBackendConfiguration {
  ExecutionProfile profile;
  ResourceLimits limits;
  AllocationFailureInjection validation_allocation_failure;
  AllocationFailureInjection runtime_allocation_failure;
};

struct RewriteEvaluationDiagnostic {
  RewriteEvaluationStage stage;
  RewriteDiagnostic rewrite;
  Error error;
  RewriteSpan primary;
  RewriteSpan context;
  RewriteSpan related;
  RewriteSpan primitive_name;
  RewriteSpan call;
  std::vector<RewriteSpan> arguments;
  ValueInvariant formatting_invariant;
  ValueFormatError formatting_error;
};

enum class RewriteCardinality {
  scalar,
  static_vector,
  dynamic_vector,
};

struct RewriteLoweringNode {
  RewriteNodeKind kind;
  RewriteCardinality cardinality;
  ScalarType element_type;
  std::size_t element_count;
  PrimitiveImplementation implementation;
  bool runtime_shape_check;
  std::size_t first_argument;
  std::size_t argument_count;
  std::size_t first_element;
  std::size_t parameter_index;
  bool boolean;
  std::int64_t integer;
  double double_precision;
  RewriteSpan source_span;
  SourceLocation source_location;
  std::string_view admission_point;
};

struct RewriteLoweringProgram {
  std::vector<RewriteLoweringNode> nodes;
  std::vector<std::size_t> arguments;
  std::vector<std::size_t> roots;
  std::vector<std::uint8_t> boolean_elements;
  std::vector<std::int64_t> integer_elements;
  std::vector<double> double_elements;
};

struct RewriteLoweringResult {
  bool ok;
  RewriteLoweringProgram program;
  RewriteEvaluationDiagnostic diagnostic;
};

struct RewriteEvaluationResult {
  bool ok;
  std::vector<Value> values;
  std::vector<std::string> formatted;
  RewriteLoweringProgram lowering;
  RewriteEvaluationDiagnostic diagnostic;
  EvaluationResources resources;
  std::size_t scalar_kernel_invocations;
};

EvaluationResources make_rewrite_resources(
    const RewriteEvaluationCreationData &creation) {
  if (creation.profile == ExecutionProfile::trusted_local_v1 &&
      !creation.limits.max_vector_bytes.has_value() &&
      !creation.limits.max_live_evaluation_bytes.has_value() &&
      !creation.limits.max_work_units.has_value()) {
    return make_trusted_local_resources(creation.allocation_failure);
  }
  return EvaluationResources{creation.profile,
                             creation.limits,
                             creation.allocation_failure,
                             0U,
                             0U,
                             0U};
}

SourceLocation rewrite_source_location(RewritePosition position) {
  return SourceLocation{position.offset, position.line, position.column};
}

RewriteEvaluationDiagnostic empty_rewrite_evaluation_diagnostic() {
  const RewriteSpan empty =
      insertion_span(RewritePosition{1U, 1U, 1U});
  return RewriteEvaluationDiagnostic{
      RewriteEvaluationStage::none,
      RewriteDiagnostic{RewriteParseError::none, empty, empty, empty},
      make_error(ErrorKind::none, rewrite_source_location(empty.begin)),
      empty,
      empty,
      empty,
      empty,
      empty,
      {},
      ValueInvariant::none,
      ValueFormatError::none};
}

RewriteEvaluationResult rewrite_evaluation_failure(
    EvaluationResources resources, RewriteEvaluationDiagnostic diagnostic,
    std::size_t scalar_kernel_invocations) {
  return RewriteEvaluationResult{false,
                                 {},
                                 {},
                                 {},
                                 std::move(diagnostic),
                                 resources,
                                 scalar_kernel_invocations};
}

Value scalar_literal_value(const RewriteNode &node) {
  if (node.element_type == ScalarType::boolean) {
    return make_bool_value(node.boolean);
  }
  if (node.element_type == ScalarType::integer) {
    return make_int_value(node.integer);
  }
  return make_double_value(node.double_precision);
}

ContainerKind lowering_container(const RewriteLoweringNode &node) {
  return node.cardinality == RewriteCardinality::scalar
             ? ContainerKind::scalar
             : ContainerKind::vector;
}

Error lowering_primitive_error(ErrorKind kind,
                               const PrimitiveDescriptor &descriptor,
                               SourceLocation location) {
  Error error = make_error(kind, location);
  error.primitive = PrimitiveErrorContext{
      std::string(descriptor.name), std::optional<PrimitiveId>{descriptor.id}};
  return error;
}

Error lowering_arity_error(const PrimitiveDescriptor &descriptor,
                           std::size_t supplied, SourceLocation location) {
  Error error =
      lowering_primitive_error(ErrorKind::arity_error, descriptor, location);
  ArityErrorContext context{supplied, {}};
  for (std::size_t index = 0U; index < descriptor.signature_count; ++index) {
    const std::size_t arity = descriptor.signatures[index].parameter_count;
    if (std::find(context.accepted.begin(), context.accepted.end(), arity) ==
        context.accepted.end()) {
      context.accepted.push_back(arity);
    }
  }
  error.arity = std::move(context);
  return error;
}

bool lowering_type_accepts(const PrimitiveDescriptor &descriptor,
                           const PrimitiveSignature &signature,
                           std::size_t argument_index,
                           const RewriteLoweringNode &argument) {
  const ValueType parameter = signature.parameters[argument_index];
  if (descriptor.lifting == LiftingMode::none) {
    return parameter.container == lowering_container(argument) &&
           parameter.element == argument.element_type;
  }
  return argument.element_type == parameter.element ||
         (argument.element_type == ScalarType::integer &&
          parameter.element == ScalarType::double_precision);
}

Error lowering_type_error(const RewriteProgram &program,
                          const RewriteCall &call,
                          const PrimitiveDescriptor &descriptor,
                          const RewriteLoweringProgram &lowering) {
  Error error = lowering_primitive_error(
      ErrorKind::type_mismatch, descriptor,
      rewrite_source_location(call.name_span.begin));
  TypeErrorContext context;
  context.actual_arguments.reserve(call.argument_count);
  for (std::size_t index = 0U; index < call.argument_count; ++index) {
    const RewriteLoweringNode &argument =
        lowering.nodes[program.arguments[call.first_argument + index]];
    context.actual_arguments.push_back(
        ErrorValueType{lowering_container(argument), argument.element_type});
  }

  std::vector<const PrimitiveSignature *> candidates;
  for (std::size_t signature_index = 0U;
       signature_index < descriptor.signature_count; ++signature_index) {
    const PrimitiveSignature &signature =
        descriptor.signatures[signature_index];
    if (signature.parameter_count != call.argument_count) {
      continue;
    }
    TypeErrorSignatureContext accepted;
    accepted.parameters.reserve(signature.parameter_count);
    for (std::size_t parameter_index = 0U;
         parameter_index < signature.parameter_count; ++parameter_index) {
      accepted.parameters.push_back(ErrorValueType{
          signature.parameters[parameter_index].container,
          signature.parameters[parameter_index].element});
    }
    accepted.result = ErrorValueType{signature.result.container,
                                     signature.result.element};
    context.accepted_signatures.push_back(std::move(accepted));
    candidates.push_back(&signature);
  }
  for (std::size_t argument_index = 0U;
       argument_index < call.argument_count && !candidates.empty();
       ++argument_index) {
    std::vector<const PrimitiveSignature *> remaining;
    const RewriteLoweringNode &argument =
        lowering.nodes[program.arguments[call.first_argument + argument_index]];
    for (const PrimitiveSignature *candidate : candidates) {
      if (lowering_type_accepts(descriptor, *candidate, argument_index,
                                argument)) {
        remaining.push_back(candidate);
      }
    }
    if (remaining.empty()) {
      error.argument_position = argument_index + 1U;
    }
    candidates = std::move(remaining);
  }
  error.type = std::move(context);
  return error;
}

RewriteEvaluationDiagnostic lowering_diagnostic(
    const RewriteProgram &program, const RewriteCall &call, Error error) {
  RewriteEvaluationDiagnostic diagnostic =
      empty_rewrite_evaluation_diagnostic();
  diagnostic.stage = RewriteEvaluationStage::application;
  diagnostic.primitive_name = call.name_span;
  diagnostic.call = call.span;
  diagnostic.context = call.span;
  diagnostic.related = call.name_span;
  diagnostic.arguments.assign(
      program.argument_spans.begin() +
          static_cast<std::ptrdiff_t>(call.first_argument),
      program.argument_spans.begin() + static_cast<std::ptrdiff_t>(
                                           call.first_argument +
                                           call.argument_count));
  diagnostic.primary = call.name_span;
  if ((error.kind == ErrorKind::type_mismatch ||
       error.kind == ErrorKind::shape_mismatch) &&
      error.argument_position.has_value() &&
      *error.argument_position >= 1U &&
      *error.argument_position <= diagnostic.arguments.size()) {
    diagnostic.primary =
        diagnostic.arguments[*error.argument_position - 1U];
  }
  error.location = rewrite_source_location(diagnostic.primary.begin);
  diagnostic.error = std::move(error);
  return diagnostic;
}

RewriteLoweringResult lowering_failure(const RewriteProgram &program,
                                       const RewriteCall &call, Error error) {
  return RewriteLoweringResult{
      false, {}, lowering_diagnostic(program, call, std::move(error))};
}

RewriteLoweringNode base_lowering_node(const RewriteNode &node) {
  const bool vector = node.kind == RewriteNodeKind::vector_literal;
  return RewriteLoweringNode{
      node.kind,
      vector ? RewriteCardinality::static_vector : RewriteCardinality::scalar,
      node.element_type,
      vector ? node.element_count : 1U,
      PrimitiveImplementation::none,
      false,
      0U,
      0U,
      node.first_element,
      node.kind == RewriteNodeKind::parameter_reference ? node.first_element
                                                        : 0U,
      node.boolean,
      node.integer,
      node.double_precision,
      node.span,
      rewrite_source_location(node.span.begin),
      vector ? std::string_view{"vector-literal"} : std::string_view{}};
}

RewriteLoweringResult lower_rewrite_program(const RewriteProgram &program) {
  RewriteLoweringProgram lowering;
  lowering.arguments = program.arguments;
  lowering.roots = program.roots;
  lowering.boolean_elements = program.boolean_elements;
  lowering.integer_elements = program.integer_elements;
  lowering.double_elements = program.double_elements;
  lowering.nodes.reserve(program.nodes.size());
  for (const RewriteNode &node : program.nodes) {
    lowering.nodes.push_back(base_lowering_node(node));
  }

  // Phase 4 is program-wide: no type or runtime work may hide a later arity
  // failure.
  for (std::size_t node_index = 0U; node_index < program.nodes.size();
       ++node_index) {
    const RewriteNode &node = program.nodes[node_index];
    if (node.kind != RewriteNodeKind::primitive_call) {
      continue;
    }
    const RewriteCall &call = program.calls[node.call_index];
    const PrimitiveDescriptor *descriptor =
        call.primitive.has_value() ? find_primitive(*call.primitive) : nullptr;
    if (descriptor == nullptr) {
      return lowering_failure(
          program, call,
          make_error(ErrorKind::invalid_primitive_table,
                     rewrite_source_location(call.name_span.begin)));
    }
    bool arity_exists = false;
    for (std::size_t signature_index = 0U;
         signature_index < descriptor->signature_count; ++signature_index) {
      if (descriptor->signatures[signature_index].parameter_count ==
          call.argument_count) {
        arity_exists = true;
        break;
      }
    }
    if (!arity_exists) {
      return lowering_failure(
          program, call,
          lowering_arity_error(*descriptor, call.argument_count,
                               rewrite_source_location(call.name_span.begin)));
    }
  }

  // Phase 5 selects every implementation from structural types only.
  for (std::size_t node_index = 0U; node_index < program.nodes.size();
       ++node_index) {
    const RewriteNode &node = program.nodes[node_index];
    if (node.kind != RewriteNodeKind::primitive_call) {
      continue;
    }
    const RewriteCall &call = program.calls[node.call_index];
    const PrimitiveDescriptor &descriptor = *find_primitive(*call.primitive);
    const PrimitiveSignature *signature = nullptr;
    if (descriptor.lifting == LiftingMode::none) {
      for (std::size_t signature_index = 0U;
           signature_index < descriptor.signature_count; ++signature_index) {
        const PrimitiveSignature &candidate =
            descriptor.signatures[signature_index];
        bool accepted = candidate.parameter_count == call.argument_count;
        for (std::size_t argument_index = 0U;
             accepted && argument_index < call.argument_count;
             ++argument_index) {
          const RewriteLoweringNode &argument = lowering.nodes[
              program.arguments[call.first_argument + argument_index]];
          accepted = lowering_type_accepts(descriptor, candidate,
                                            argument_index, argument);
        }
        if (accepted) {
          signature = &candidate;
          break;
        }
      }
    } else {
      std::array<ScalarType, 2> actual_types{};
      if (call.argument_count <= actual_types.size()) {
        for (std::size_t argument_index = 0U;
             argument_index < call.argument_count; ++argument_index) {
          actual_types[argument_index] =
              lowering
                  .nodes[program.arguments[call.first_argument + argument_index]]
                  .element_type;
        }
        const SignatureSelectionResult selected = select_primitive_signature(
            descriptor,
            std::span<const ScalarType>(actual_types.data(),
                                        call.argument_count));
        if (selected.status == SignatureSelectionStatus::success) {
          signature = selected.signature;
        }
      }
    }
    if (signature == nullptr) {
      return lowering_failure(
          program, call,
          lowering_type_error(program, call, descriptor, lowering));
    }
    RewriteLoweringNode &lowered = lowering.nodes[node_index];
    lowered.implementation = signature->implementation;
    lowered.element_type = signature->result.element;
    lowered.first_argument = call.first_argument;
    lowered.argument_count = call.argument_count;
    lowered.source_location = rewrite_source_location(call.name_span.begin);
    lowered.admission_point = descriptor.name;
    lowered.cardinality = descriptor.lifting == LiftingMode::none
                              ? RewriteCardinality::dynamic_vector
                              : RewriteCardinality::scalar;
  }

  // Phase 6 computes cardinality and rejects only lengths proven unequal.
  for (std::size_t node_index = 0U; node_index < program.nodes.size();
       ++node_index) {
    const RewriteNode &node = program.nodes[node_index];
    if (node.kind != RewriteNodeKind::primitive_call) {
      continue;
    }
    const RewriteCall &call = program.calls[node.call_index];
    RewriteLoweringNode &lowered = lowering.nodes[node_index];
    if (lowered.implementation == PrimitiveImplementation::iota_integer) {
      lowered.cardinality = RewriteCardinality::dynamic_vector;
      lowered.element_count = 0U;
      continue;
    }
    std::optional<std::size_t> known_length;
    bool has_dynamic = false;
    std::size_t vector_count = 0U;
    for (std::size_t argument_index = 0U;
         argument_index < call.argument_count; ++argument_index) {
      const RewriteLoweringNode &argument = lowering.nodes[
          program.arguments[call.first_argument + argument_index]];
      if (argument.cardinality == RewriteCardinality::scalar) {
        continue;
      }
      ++vector_count;
      if (argument.cardinality == RewriteCardinality::dynamic_vector) {
        has_dynamic = true;
        continue;
      }
      if (!known_length.has_value()) {
        known_length = argument.element_count;
      } else if (*known_length != argument.element_count) {
        Error error = lowering_primitive_error(
            ErrorKind::shape_mismatch, *find_primitive(*call.primitive),
            rewrite_source_location(call.name_span.begin));
        error.argument_position = argument_index + 1U;
        error.shape = ShapeErrorContext{{*known_length},
                                        {argument.element_count}};
        return lowering_failure(program, call, std::move(error));
      }
    }
    lowered.runtime_shape_check = vector_count > 1U && has_dynamic;
    if (vector_count == 0U) {
      lowered.cardinality = RewriteCardinality::scalar;
      lowered.element_count = 1U;
    } else if (has_dynamic) {
      lowered.cardinality = RewriteCardinality::dynamic_vector;
      lowered.element_count = known_length.value_or(0U);
    } else {
      lowered.cardinality = RewriteCardinality::static_vector;
      lowered.element_count = *known_length;
    }
  }
  return RewriteLoweringResult{
      true, std::move(lowering), empty_rewrite_evaluation_diagnostic()};
}

VectorAllocationResult vector_literal_value(EvaluationResources &resources,
                                             const RewriteProgram &program,
                                             const RewriteNode &node) {
  const SourceLocation location = rewrite_source_location(node.span.begin);
  if (node.element_type == ScalarType::boolean) {
    return copy_bool_vector(
        resources,
        std::span<const std::uint8_t>(program.boolean_elements)
            .subspan(node.first_element, node.element_count),
        location, "vector-literal");
  }
  if (node.element_type == ScalarType::integer) {
    return copy_int_vector(
        resources,
        std::span<const std::int64_t>(program.integer_elements)
            .subspan(node.first_element, node.element_count),
        location, "vector-literal");
  }
  return copy_double_vector(
      resources,
      std::span<const double>(program.double_elements)
          .subspan(node.first_element, node.element_count),
      location, "vector-literal");
}

void release_rewrite_values(EvaluationResources &resources,
                            std::vector<Value> &values) {
  for (Value &value : values) {
    if (value.container == ContainerKind::vector) {
      release_vector_reservation(resources, value);
    } else {
      destroy_value(value);
    }
  }
  values.clear();
}

void release_rewrite_node_values(EvaluationResources &resources,
                                 std::vector<Value> &values,
                                 std::vector<std::uint8_t> &live) {
  for (std::size_t index = 0U; index < values.size(); ++index) {
    if (live[index] == std::uint8_t{0U}) {
      continue;
    }
    if (values[index].container == ContainerKind::vector) {
      release_vector_reservation(resources, values[index]);
    } else {
      destroy_value(values[index]);
    }
    live[index] = std::uint8_t{0U};
  }
}

RewriteEvaluationDiagnostic application_rewrite_diagnostic(
    const RewriteProgram &program, const RewriteCall &call, Error error) {
  RewriteEvaluationDiagnostic diagnostic =
      empty_rewrite_evaluation_diagnostic();
  diagnostic.stage = RewriteEvaluationStage::application;
  diagnostic.primitive_name = call.name_span;
  diagnostic.call = call.span;
  diagnostic.context = call.span;
  diagnostic.related = call.name_span;
  diagnostic.arguments.assign(
      program.argument_spans.begin() +
          static_cast<std::ptrdiff_t>(call.first_argument),
      program.argument_spans.begin() + static_cast<std::ptrdiff_t>(
                                           call.first_argument +
                                           call.argument_count));

  diagnostic.primary = call.name_span;
  if ((error.kind == ErrorKind::type_mismatch ||
       error.kind == ErrorKind::shape_mismatch) &&
      error.argument_position.has_value() &&
      *error.argument_position >= 1U &&
      *error.argument_position <= diagnostic.arguments.size()) {
    diagnostic.primary =
        diagnostic.arguments[*error.argument_position - 1U];
  }
  error.location = rewrite_source_location(diagnostic.primary.begin);
  diagnostic.error = std::move(error);
  return diagnostic;
}

std::optional<Error> rewrite_runtime_shape_error(
    const RewriteLoweringNode &call,
    const RewriteLoweringProgram &lowering,
    const PrimitiveDescriptor &descriptor, std::span<const Value> arguments) {
  if (!call.runtime_shape_check) {
    return std::nullopt;
  }

  std::optional<std::size_t> expected_count;
  for (std::size_t position = 0U; position < call.argument_count; ++position) {
    const std::size_t argument_node =
        lowering.arguments[call.first_argument + position];
    if (lowering.nodes[argument_node].cardinality ==
        RewriteCardinality::static_vector) {
      expected_count = call.element_count;
      break;
    }
  }

  for (std::size_t position = 0U; position < call.argument_count; ++position) {
    const std::size_t argument_node =
        lowering.arguments[call.first_argument + position];
    if (lowering.nodes[argument_node].cardinality !=
        RewriteCardinality::dynamic_vector) {
      continue;
    }
    std::size_t actual_count = 0U;
    if (!value_length(arguments[position], actual_count).ok) {
      continue;
    }
    if (!expected_count.has_value()) {
      expected_count = actual_count;
      continue;
    }
    if (actual_count == *expected_count) {
      continue;
    }
    Error error = lowering_primitive_error(
        ErrorKind::shape_mismatch, descriptor, call.source_location);
    error.argument_position = position + 1U;
    error.shape = ShapeErrorContext{{*expected_count}, {actual_count}};
    return error;
  }
  return std::nullopt;
}

bool format_rewrite_root_values(const RewriteProgram &program,
                                const std::vector<Value> &values,
                                std::vector<std::string> &formatted,
                                RewriteEvaluationDiagnostic &diagnostic) {
  formatted.clear();
  if (values.size() != program.roots.size()) {
    diagnostic = empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::formatting;
    diagnostic.formatting_error = ValueFormatError::invalid_value;
    return false;
  }
  formatted.reserve(values.size());
  for (std::size_t index = 0U; index < values.size(); ++index) {
    ValueFormattingResult result = format_value(values[index]);
    if (!result.ok) {
      formatted.clear();
      diagnostic = empty_rewrite_evaluation_diagnostic();
      diagnostic.stage = RewriteEvaluationStage::formatting;
      diagnostic.primary = program.nodes[program.roots[index]].span;
      diagnostic.context = diagnostic.primary;
      diagnostic.formatting_invariant = result.invariant;
      diagnostic.formatting_error = result.error;
      return false;
    }
    formatted.push_back(std::move(result.formatted));
  }
  return true;
}

void release_rewrite_evaluation_result(RewriteEvaluationResult &result) {
  release_rewrite_values(result.resources, result.values);
  result.formatted.clear();
}

std::string_view resource_reason_name(ResourceErrorReason reason) {
  switch (reason) {
  case ResourceErrorReason::size_overflow:
    return "size_overflow";
  case ResourceErrorReason::profile_limit:
    return "profile_limit";
  case ResourceErrorReason::allocation_unavailable:
    return "allocation_unavailable";
  }
  return "unknown_resource_failure";
}

std::string_view domain_reason_name(DomainErrorReason reason) {
  switch (reason) {
  case DomainErrorReason::integer_overflow:
    return "integer_overflow";
  }
  return "unknown_domain_failure";
}

std::string source_at_span(std::string_view source, RewriteSpan span) {
  if (span.begin.offset == 0U || span.end.offset < span.begin.offset ||
      span.end.offset - 1U > source.size()) {
    return {};
  }
  const std::size_t begin = span.begin.offset - 1U;
  return std::string(source.substr(begin, span.end.offset - span.begin.offset));
}

std::string parse_error_message(RewriteParseError error) {
  switch (error) {
  case RewriteParseError::none:
    return {};
  case RewriteParseError::invalid_byte:
    return "invalid source byte";
  case RewriteParseError::malformed_literal:
    return "malformed scalar literal";
  case RewriteParseError::literal_range:
    return "scalar literal is outside its accepted range";
  case RewriteParseError::expected_expression:
    return "expected an expression";
  case RewriteParseError::primitive_requires_application:
    return "primitive name requires bracketed or unary prefix application";
  case RewriteParseError::whitespace_before_bracket:
    return "whitespace is not allowed before '['";
  case RewriteParseError::missing_separator:
    return "sibling expressions require separating whitespace";
  case RewriteParseError::mismatched_delimiter:
    return "mismatched closing delimiter";
  case RewriteParseError::missing_delimiter:
    return "missing closing delimiter";
  case RewriteParseError::bare_empty_vector:
    return "empty vector requires Bool(), Int(), or Double()";
  case RewriteParseError::heterogeneous_vector:
    return "vector elements must have one scalar type";
  case RewriteParseError::invalid_vector_element:
    return "vector elements must be scalar literals";
  case RewriteParseError::trailing_input:
    return "root expression has trailing input";
  case RewriteParseError::unknown_primitive:
    return "unknown primitive";
  }
  return "invalid source";
}

ErrorKind parse_error_kind(RewriteParseError error) {
  if (error == RewriteParseError::invalid_byte) {
    return ErrorKind::invalid_byte;
  }
  if (error == RewriteParseError::malformed_literal) {
    return ErrorKind::malformed_literal;
  }
  if (error == RewriteParseError::literal_range) {
    return ErrorKind::literal_range_error;
  }
  if (error == RewriteParseError::unknown_primitive) {
    return ErrorKind::unknown_name;
  }
  return ErrorKind::syntax_error;
}

std::string semantic_error_message(const Error &error) {
  const std::string primitive =
      error.primitive.has_value() ? error.primitive->name : "evaluation";
  if (error.kind == ErrorKind::arity_error && error.arity.has_value()) {
    std::string message = primitive + " received " +
                          std::to_string(error.arity->supplied) +
                          " argument(s); accepted arity";
    if (error.arity->accepted.size() != 1U) {
      message += " values";
    }
    for (const std::size_t accepted : error.arity->accepted) {
      message += " " + std::to_string(accepted);
    }
    return message;
  }
  if (error.kind == ErrorKind::type_mismatch) {
    std::string message = primitive + " arguments do not match an accepted signature";
    if (error.argument_position.has_value()) {
      message += "; first unsupported argument is " +
                 std::to_string(*error.argument_position);
    }
    return message;
  }
  if (error.kind == ErrorKind::shape_mismatch && error.shape.has_value()) {
    const std::size_t expected = error.shape->expected.empty()
                                     ? 0U
                                     : error.shape->expected.front();
    const std::size_t actual =
        error.shape->actual.empty() ? 0U : error.shape->actual.front();
    return primitive + " argument " +
           std::to_string(error.argument_position.value_or(0U)) +
           " expected shape [" + std::to_string(expected) + "], got [" +
           std::to_string(actual) + "]";
  }
  if (error.kind == ErrorKind::resource_error && error.resource.has_value()) {
    return primitive + " resource request failed: " +
           std::string(resource_reason_name(error.resource->reason));
  }
  if (error.kind == ErrorKind::domain_error && error.domain.has_value()) {
    std::string message = primitive + " failed: " +
                          std::string(domain_reason_name(error.domain->reason));
    if (error.element_index.has_value()) {
      message += " at result index " + std::to_string(*error.element_index);
    }
    return message;
  }
  if (error.kind == ErrorKind::invalid_primitive_table) {
    return "built-in primitive table is invalid";
  }
  if (error.kind == ErrorKind::formatting_error) {
    return "evaluated value cannot be formatted canonically";
  }
  return "evaluation failed";
}

Error public_error_from_diagnostic(
    std::string_view source, const RewriteEvaluationDiagnostic &diagnostic) {
  if (diagnostic.error.kind != ErrorKind::none) {
    Error error = diagnostic.error;
    if (error.kind == ErrorKind::unknown_name &&
        !error.primitive.has_value()) {
      const std::string name = source_at_span(source, diagnostic.primary);
      error.primitive = PrimitiveErrorContext{name, std::nullopt};
      error.message = "unknown primitive '" + name + "'";
    }
    if (error.message.empty()) {
      error.message = semantic_error_message(error);
    }
    return error;
  }

  if (diagnostic.stage == RewriteEvaluationStage::parse ||
      diagnostic.stage == RewriteEvaluationStage::resolution) {
    const RewriteParseError parse_error = diagnostic.rewrite.error;
    Error error = make_error(
        parse_error_kind(parse_error),
        rewrite_source_location(diagnostic.primary.begin),
        parse_error_message(parse_error));
    if (parse_error == RewriteParseError::unknown_primitive) {
      const std::string name = source_at_span(source, diagnostic.primary);
      error.primitive = PrimitiveErrorContext{name, std::nullopt};
      error.message += " '" + name + "'";
    }
    return error;
  }

  if (diagnostic.stage == RewriteEvaluationStage::formatting) {
    return make_error(ErrorKind::formatting_error,
                      rewrite_source_location(diagnostic.primary.begin),
                      "evaluated value cannot be formatted canonically");
  }
  return make_error(ErrorKind::invalid_primitive_table,
                    rewrite_source_location(diagnostic.primary.begin),
                    "rewrite evaluation failed internally");
}

CBackendConfiguration trusted_local_c_configuration() {
  return CBackendConfiguration{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt},
      AllocationFailureInjection{std::nullopt}};
}

EvaluationConfiguration trusted_local_evaluation_configuration() {
  return EvaluationConfiguration{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
}

CBackendConfiguration c_backend_configuration(
    const EvaluationConfiguration &configuration) {
  return CBackendConfiguration{
      configuration.profile, configuration.limits,
      AllocationFailureInjection{std::nullopt},
      configuration.allocation_failure};
}

RewriteEvaluationResult evaluate_rewrite_source_impl(
    std::string_view source, const RewriteEvaluationCreationData &creation,
    bool require_single_root) {
  EvaluationResources resources = make_rewrite_resources(creation);
  RewriteParseResult parsed = parse_rewrite(source);
  if (!parsed.ok) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::parse;
    diagnostic.rewrite = parsed.diagnostic;
    diagnostic.primary = parsed.diagnostic.primary;
    diagnostic.context = parsed.diagnostic.context;
    diagnostic.related = parsed.diagnostic.related;
    return rewrite_evaluation_failure(resources, std::move(diagnostic), 0U);
  }

  if (require_single_root && parsed.program.roots.size() != 1U) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::parse;
    if (parsed.program.roots.empty()) {
      diagnostic.primary = parsed.diagnostic.primary;
      diagnostic.context = parsed.diagnostic.context;
      diagnostic.error = make_error(
          ErrorKind::empty_expression,
          rewrite_source_location(parsed.diagnostic.primary.begin),
          "expected one expression");
    } else {
      diagnostic.primary =
          parsed.program.nodes[parsed.program.roots[1U]].span;
      diagnostic.context = diagnostic.primary;
      diagnostic.error = make_error(
          ErrorKind::syntax_error,
          rewrite_source_location(diagnostic.primary.begin),
          "evaluate_expression accepts exactly one root expression");
    }
    return rewrite_evaluation_failure(resources, std::move(diagnostic), 0U);
  }

  const RewriteResolutionResult resolved =
      resolve_rewrite_primitives(parsed.program);
  if (!resolved.ok) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::resolution;
    diagnostic.rewrite = resolved.diagnostic;
    diagnostic.primary = resolved.diagnostic.primary;
    diagnostic.context = resolved.diagnostic.context;
    diagnostic.related = resolved.diagnostic.related;
    diagnostic.error = make_error(
        ErrorKind::unknown_name,
        rewrite_source_location(resolved.diagnostic.primary.begin));
    return rewrite_evaluation_failure(resources, std::move(diagnostic), 0U);
  }

  if (!production_primitive_table_validation().ok) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::primitive_table;
    diagnostic.error = make_error(
        ErrorKind::invalid_primitive_table, SourceLocation{1U, 1U, 1U});
    return rewrite_evaluation_failure(resources, std::move(diagnostic), 0U);
  }

  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  if (!lowered.ok) {
    return rewrite_evaluation_failure(resources, std::move(lowered.diagnostic),
                                      0U);
  }

  WorkChargeResult resource_admission = charge_work(
      resources, 0U, SourceLocation{1U, 1U, 1U}, "rewrite-evaluator");
  if (!resource_admission.ok) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::resource_admission;
    diagnostic.error = std::move(resource_admission.error);
    return rewrite_evaluation_failure(resources, std::move(diagnostic), 0U);
  }

  std::size_t maximum_call_arity = 0U;
  for (const RewriteCall &call : parsed.program.calls) {
    if (call.argument_count > maximum_call_arity) {
      maximum_call_arity = call.argument_count;
    }
  }
  std::vector<Value> arguments;
  arguments.reserve(maximum_call_arity);

  std::vector<std::size_t> remaining_uses(parsed.program.nodes.size(), 0U);
  for (const std::size_t argument : parsed.program.arguments) {
    ++remaining_uses[argument];
  }
  for (const std::size_t root : parsed.program.roots) {
    ++remaining_uses[root];
  }

  std::vector<Value> node_values;
  node_values.reserve(parsed.program.nodes.size());
  for (std::size_t index = 0U; index < parsed.program.nodes.size(); ++index) {
    node_values.push_back(make_int_value(0));
  }
  std::vector<std::uint8_t> node_live(parsed.program.nodes.size(),
                                      std::uint8_t{0U});
  std::size_t scalar_kernel_invocations = 0U;
  RewriteLoweringProgram lowering = std::move(lowered.program);

  for (std::size_t node_index = 0U;
       node_index < parsed.program.nodes.size(); ++node_index) {
    const RewriteNode &node = parsed.program.nodes[node_index];
    if (node.kind == RewriteNodeKind::scalar_literal) {
      node_values[node_index] = scalar_literal_value(node);
      node_live[node_index] = std::uint8_t{1U};
      continue;
    }
    if (node.kind == RewriteNodeKind::vector_literal) {
      VectorAllocationResult literal =
          vector_literal_value(resources, parsed.program, node);
      if (literal.ok) {
        node_values[node_index] = std::move(literal.value);
        node_live[node_index] = std::uint8_t{1U};
        continue;
      }
      RewriteEvaluationDiagnostic diagnostic =
          empty_rewrite_evaluation_diagnostic();
      diagnostic.stage = RewriteEvaluationStage::literal;
      diagnostic.primary = node.span;
      diagnostic.context = node.span;
      diagnostic.error = std::move(literal.error);
      release_rewrite_node_values(resources, node_values, node_live);
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }

    const RewriteCall &call = parsed.program.calls[node.call_index];
    const PrimitiveDescriptor *descriptor =
        call.primitive.has_value() ? find_primitive(*call.primitive) : nullptr;
    if (descriptor == nullptr) {
      Error error = make_error(ErrorKind::invalid_primitive_table,
                               rewrite_source_location(call.name_span.begin));
      RewriteEvaluationDiagnostic diagnostic =
          application_rewrite_diagnostic(parsed.program, call,
                                         std::move(error));
      release_rewrite_node_values(resources, node_values, node_live);
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }

    bool invalid_forward_use = false;
    for (std::size_t argument_index = 0U;
         argument_index < call.argument_count; ++argument_index) {
      const std::size_t argument_node =
          parsed.program.arguments[call.first_argument + argument_index];
      if (node_live[argument_node] == std::uint8_t{0U} ||
          remaining_uses[argument_node] != 1U) {
        invalid_forward_use = true;
        break;
      }
      arguments.push_back(std::move(node_values[argument_node]));
      node_live[argument_node] = std::uint8_t{0U};
      --remaining_uses[argument_node];
    }
    if (invalid_forward_use) {
      release_rewrite_values(resources, arguments);
      Error error = make_error(ErrorKind::invalid_primitive_table,
                               rewrite_source_location(call.name_span.begin));
      RewriteEvaluationDiagnostic diagnostic =
          application_rewrite_diagnostic(parsed.program, call,
                                         std::move(error));
      release_rewrite_node_values(resources, node_values, node_live);
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }

    PrimitiveApplicationContext application_context{
        resources, scalar_kernel_invocations};
    std::optional<Error> shape_error = rewrite_runtime_shape_error(
        lowering.nodes[node_index], lowering, *descriptor, arguments);
    if (shape_error.has_value()) {
      RewriteEvaluationDiagnostic diagnostic =
          application_rewrite_diagnostic(parsed.program, call,
                                         std::move(*shape_error));
      release_rewrite_values(resources, arguments);
      release_rewrite_node_values(resources, node_values, node_live);
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }
    PrimitiveApplicationResult applied = apply_typed_primitive(
        application_context, *descriptor,
        lowering.nodes[node_index].implementation, arguments,
        rewrite_source_location(call.name_span.begin));
    scalar_kernel_invocations = application_context.scalar_kernel_invocations;
    if (!applied.ok) {
      RewriteEvaluationDiagnostic diagnostic =
          application_rewrite_diagnostic(parsed.program, call,
                                         std::move(applied.error));
      release_rewrite_values(resources, arguments);
      release_rewrite_node_values(resources, node_values, node_live);
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }
    release_rewrite_values(resources, arguments);
    node_values[node_index] = std::move(applied.value);
    node_live[node_index] = std::uint8_t{1U};
  }

  std::vector<Value> values;
  values.reserve(parsed.program.roots.size());
  for (const std::size_t root : parsed.program.roots) {
    if (node_live[root] == std::uint8_t{0U} || remaining_uses[root] != 1U) {
      release_rewrite_values(resources, values);
      release_rewrite_node_values(resources, node_values, node_live);
      RewriteEvaluationDiagnostic diagnostic =
          empty_rewrite_evaluation_diagnostic();
      diagnostic.stage = RewriteEvaluationStage::primitive_table;
      diagnostic.error = make_error(ErrorKind::invalid_primitive_table,
                                     SourceLocation{1U, 1U, 1U});
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }
    values.push_back(std::move(node_values[root]));
    node_live[root] = std::uint8_t{0U};
    --remaining_uses[root];
  }

  std::vector<std::string> formatted;
  RewriteEvaluationDiagnostic formatting_diagnostic =
      empty_rewrite_evaluation_diagnostic();
  if (!format_rewrite_root_values(parsed.program, values, formatted,
                                  formatting_diagnostic)) {
    release_rewrite_values(resources, values);
    release_rewrite_node_values(resources, node_values, node_live);
    return rewrite_evaluation_failure(
        resources, std::move(formatting_diagnostic),
        scalar_kernel_invocations);
  }

  return RewriteEvaluationResult{true,
                                 std::move(values),
                                 std::move(formatted),
                                 std::move(lowering),
                                 empty_rewrite_evaluation_diagnostic(),
                                 resources,
                                 scalar_kernel_invocations};
}

RewriteEvaluationResult evaluate_rewrite_source(
    std::string_view source, const RewriteEvaluationCreationData &creation) {
  return evaluate_rewrite_source_impl(source, creation, false);
}

void append_c_unsigned(std::string &source, std::size_t value) {
  std::array<char, 32> digits{};
  const std::to_chars_result converted =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  source.append(digits.data(), converted.ptr);
  source.push_back('U');
}

void append_c_integer(std::string &source, std::int64_t value) {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    source += "(-INT64_C(9223372036854775807) - INT64_C(1))";
    return;
  }
  if (value < 0) {
    source += "(-INT64_C(";
    value = -value;
    std::array<char, 32> digits{};
    const std::to_chars_result converted =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    source.append(digits.data(), converted.ptr);
    source += "))";
    return;
  }
  source += "INT64_C(";
  std::array<char, 32> digits{};
  const std::to_chars_result converted =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  source.append(digits.data(), converted.ptr);
  source.push_back(')');
}

void append_c_double_bits(std::string &source, double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  std::array<char, 32> digits{};
  const std::to_chars_result converted = std::to_chars(
      digits.data(), digits.data() + digits.size(), bits, 16);
  source += "UINT64_C(0x";
  const std::size_t digit_count =
      static_cast<std::size_t>(converted.ptr - digits.data());
  source.append(16U - digit_count, '0');
  source.append(digits.data(), converted.ptr);
  source.push_back(')');
}

std::string_view c_implementation_name(PrimitiveImplementation implementation) {
  switch (implementation) {
  case PrimitiveImplementation::inc_integer:
    return "BENNU_IMPL_INC_INT";
  case PrimitiveImplementation::inc_double:
    return "BENNU_IMPL_INC_DOUBLE";
  case PrimitiveImplementation::add_integer:
    return "BENNU_IMPL_ADD_INT";
  case PrimitiveImplementation::add_double:
    return "BENNU_IMPL_ADD_DOUBLE";
  case PrimitiveImplementation::equals_boolean:
    return "BENNU_IMPL_EQUALS_BOOL";
  case PrimitiveImplementation::equals_integer:
    return "BENNU_IMPL_EQUALS_INT";
  case PrimitiveImplementation::equals_double:
    return "BENNU_IMPL_EQUALS_DOUBLE";
  case PrimitiveImplementation::logical_not_boolean:
    return "BENNU_IMPL_NOT_BOOL";
  case PrimitiveImplementation::iota_integer:
    return "BENNU_IMPL_IOTA_INT";
  case PrimitiveImplementation::none:
    break;
  }
  return "0";
}

std::string_view c_type_name(ScalarType type) {
  if (type == ScalarType::boolean) {
    return "BENNU_BOOL";
  }
  if (type == ScalarType::integer) {
    return "BENNU_INT";
  }
  return "BENNU_DOUBLE";
}

void append_literal_arrays(std::string &source,
                           const RewriteLoweringProgram &program) {
  for (std::size_t node_index = 0U; node_index < program.nodes.size();
       ++node_index) {
    const RewriteLoweringNode &node = program.nodes[node_index];
    if (node.kind != RewriteNodeKind::vector_literal ||
        node.element_count == 0U) {
      continue;
    }
    source += "static const ";
    if (node.element_type == ScalarType::boolean) {
      source += "uint8_t";
    } else if (node.element_type == ScalarType::integer) {
      source += "int64_t";
    } else {
      source += "uint64_t";
    }
    source += " bennu_literal_" + std::to_string(node_index) + "[] = {";
    for (std::size_t index = 0U; index < node.element_count; ++index) {
      if (index != 0U) {
        source += ", ";
      }
      const std::size_t payload_index = node.first_element + index;
      if (node.element_type == ScalarType::boolean) {
        source += program.boolean_elements[payload_index] == std::uint8_t{0U}
                      ? "UINT8_C(0)"
                      : "UINT8_C(1)";
      } else if (node.element_type == ScalarType::integer) {
        append_c_integer(source, program.integer_elements[payload_index]);
      } else {
        append_c_double_bits(source, program.double_elements[payload_index]);
      }
    }
    source += "};\n";
  }
  if (!program.nodes.empty()) {
    source.push_back('\n');
  }
}

void append_resource_initialization(
    std::string &source, const CBackendConfiguration &configuration) {
  const auto append_presence = [&source](const std::optional<std::size_t> &limit) {
    source += limit.has_value() ? "1, " : "0, ";
  };
  const auto append_value = [&source](const std::optional<std::size_t> &limit) {
    append_c_unsigned(source, limit.value_or(0U));
    source += ", ";
  };
  source += "  BennuResources bennu_resources = {";
  append_presence(configuration.limits.max_vector_bytes);
  append_presence(configuration.limits.max_live_evaluation_bytes);
  append_presence(configuration.limits.max_work_units);
  append_value(configuration.limits.max_vector_bytes);
  append_value(configuration.limits.max_live_evaluation_bytes);
  append_value(configuration.limits.max_work_units);
  source += "0U, 0U, 0U, ";
  source += configuration.runtime_allocation_failure
                        .fail_at_reservation_ordinal.has_value()
                ? "1, "
                : "0, ";
  append_c_unsigned(
      source,
      configuration.runtime_allocation_failure.fail_at_reservation_ordinal
          .value_or(0U));
  source += ", BENNU_FAILURE_NONE, ";
  source += configuration.profile == ExecutionProfile::bounded_v1
                ? "BENNU_PROFILE_BOUNDED_V1, "
                : "BENNU_PROFILE_TRUSTED_LOCAL_V1, ";
  source +=
      "BENNU_LIMIT_NONE, 0U, 0U, 0U, NULL, {0U, 1U, 1U}, 0, 0U};\n";
}

void append_source_location(std::string &source, SourceLocation location) {
  source += "bennu_source_location(";
  append_c_unsigned(source, location.offset);
  source += ", ";
  append_c_unsigned(source, location.line);
  source += ", ";
  append_c_unsigned(source, location.column);
  source.push_back(')');
}

void append_scalar_node(std::string &source, std::size_t node_index,
                        const RewriteLoweringNode &node) {
  source += "  bennu_values[" + std::to_string(node_index) + "] = ";
  if (node.element_type == ScalarType::boolean) {
    source += node.boolean ? "bennu_scalar_bool(UINT8_C(1));\n"
                           : "bennu_scalar_bool(UINT8_C(0));\n";
  } else if (node.element_type == ScalarType::integer) {
    source += "bennu_scalar_int(";
    append_c_integer(source, node.integer);
    source += ");\n";
  } else {
    source += "bennu_scalar_double_bits(";
    append_c_double_bits(source, node.double_precision);
    source += ");\n";
  }
}

void append_vector_node(std::string &source, std::size_t node_index,
                        const RewriteLoweringNode &node) {
  source += "  if (!bennu_literal(&bennu_resources, &bennu_values[" +
            std::to_string(node_index) + "], ";
  source += c_type_name(node.element_type);
  source += ", ";
  source += node.element_count == 0U
                ? "NULL"
                : "bennu_literal_" + std::to_string(node_index);
  source += ", ";
  append_c_unsigned(source, node.element_count);
  source += ", \"";
  source += node.admission_point;
  source += "\", ";
  append_source_location(source, node.source_location);
  source += ")) { goto bennu_failure; }\n";
}

void append_shape_requirement(
    std::string &source, const RewriteLoweringNode &call,
    const RewriteLoweringNode &argument, std::size_t argument_node,
    std::size_t argument_position, std::optional<std::size_t> static_count,
    std::optional<std::size_t> dynamic_anchor) {
  source += "  if (!bennu_require_shape(&bennu_resources, \"";
  source += call.admission_point;
  source += "\", ";
  append_c_unsigned(source, argument_position);
  source += ", ";
  if (static_count.has_value()) {
    append_c_unsigned(source, *static_count);
  } else {
    source += "bennu_values[" + std::to_string(*dynamic_anchor) + "].count";
  }
  source += ", &bennu_values[" + std::to_string(argument_node) + "], ";
  append_source_location(source, argument.source_location);
  source += ")) { goto bennu_failure; }\n";
}

void append_call_shape_checks(std::string &source,
                              const RewriteLoweringNode &call,
                              const RewriteLoweringProgram &program) {
  if (!call.runtime_shape_check) {
    return;
  }

  bool has_static_anchor = false;
  for (std::size_t position = 0U; position < call.argument_count; ++position) {
    const std::size_t argument_node =
        program.arguments[call.first_argument + position];
    if (program.nodes[argument_node].cardinality ==
        RewriteCardinality::static_vector) {
      has_static_anchor = true;
      break;
    }
  }

  std::optional<std::size_t> dynamic_anchor;
  for (std::size_t position = 0U; position < call.argument_count; ++position) {
    const std::size_t argument_node =
        program.arguments[call.first_argument + position];
    const RewriteLoweringNode &argument = program.nodes[argument_node];
    if (argument.cardinality != RewriteCardinality::dynamic_vector) {
      continue;
    }
    if (!has_static_anchor && !dynamic_anchor.has_value()) {
      dynamic_anchor = argument_node;
      continue;
    }
    append_shape_requirement(
        source, call, argument, argument_node, position + 1U,
        has_static_anchor ? std::optional<std::size_t>{call.element_count}
                          : std::nullopt,
        dynamic_anchor);
  }
}

void append_call_node(std::string &source, std::size_t node_index,
                      const RewriteLoweringNode &node,
                      const RewriteLoweringProgram &program) {
  const std::size_t left = program.arguments[node.first_argument];
  const std::size_t right = node.argument_count == 2U
                                ? program.arguments[node.first_argument + 1U]
                                : 0U;
  append_call_shape_checks(source, node, program);
  source += "  if (!bennu_apply(&bennu_resources, ";
  source += c_implementation_name(node.implementation);
  source += ", &bennu_values[" + std::to_string(node_index) +
            "], &bennu_values[" + std::to_string(left) + "], ";
  source += node.argument_count == 2U
                ? "&bennu_values[" + std::to_string(right) + "]"
                : "NULL";
  source += ", ";
  append_c_unsigned(source, node.argument_count);
  source += ", \"";
  source += node.admission_point;
  source += "\", ";
  append_source_location(source, node.source_location);
  source += ")) { goto bennu_failure; }\n";
  for (std::size_t argument = 0U; argument < node.argument_count; ++argument) {
    const std::size_t argument_node =
        program.arguments[node.first_argument + argument];
    source += "  bennu_release(&bennu_resources, &bennu_values[" +
              std::to_string(argument_node) + "]);\n";
  }
}

CEmissionResult emit_rewrite_c_source_impl(
    std::string_view source,
    const CBackendConfiguration &configuration) {
  RewriteParseResult parsed = parse_rewrite(source);
  if (!parsed.ok) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::parse;
    diagnostic.rewrite = parsed.diagnostic;
    diagnostic.primary = parsed.diagnostic.primary;
    diagnostic.context = parsed.diagnostic.context;
    diagnostic.related = parsed.diagnostic.related;
    return CEmissionResult{
        false, {}, public_error_from_diagnostic(source, diagnostic)};
  }
  RewriteResolutionResult resolution =
      resolve_rewrite_primitives(parsed.program);
  if (!resolution.ok) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::resolution;
    diagnostic.rewrite = resolution.diagnostic;
    diagnostic.primary = resolution.diagnostic.primary;
    diagnostic.context = resolution.diagnostic.context;
    diagnostic.related = resolution.diagnostic.related;
    return CEmissionResult{
        false, {}, public_error_from_diagnostic(source, diagnostic)};
  }
  if (!production_primitive_table_validation().ok) {
    return CEmissionResult{
        false, {},
        make_error(ErrorKind::invalid_primitive_table,
                   SourceLocation{1U, 1U, 1U})};
  }
  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  if (!lowered.ok) {
    return CEmissionResult{
        false, {}, public_error_from_diagnostic(source, lowered.diagnostic)};
  }
  if (lowered.program.nodes.empty() && !lowered.program.roots.empty()) {
    return CEmissionResult{
        false, {},
        make_error(ErrorKind::invalid_primitive_table,
                   SourceLocation{1U, 1U, 1U},
                   "typed rewrite lowering is internally inconsistent")};
  }
  EvaluationResources validation_resources = make_rewrite_resources(
      RewriteEvaluationCreationData{configuration.profile, configuration.limits,
                                    std::nullopt});
  WorkChargeResult resource_validation = charge_work(
      validation_resources, 0U, SourceLocation{1U, 1U, 1U}, "rewrite-emitter");
  if (!resource_validation.ok) {
    return CEmissionResult{false, {}, std::move(resource_validation.error)};
  }
  const RewriteLoweringProgram &lowering = lowered.program;

  std::string generated;
  append_rewrite_c_runtime(generated);
  append_literal_arrays(generated, lowering);
  generated += "int main(void) {\n";
  append_resource_initialization(generated, configuration);
  generated += "  (void)bennu_literal;\n"
               "  (void)bennu_apply;\n"
               "  (void)bennu_require_shape;\n"
               "  (void)bennu_source_location;\n"
               "  (void)bennu_print_value;\n";
  if (!lowering.nodes.empty()) {
    generated += "  static BennuValue bennu_values[" +
                 std::to_string(lowering.nodes.size()) +
                 "] = {{0}};\n";
  }
  generated +=
      "  if (setlocale(LC_NUMERIC, \"C\") == NULL) {\n"
      "    bennu_set_failure(&bennu_resources, BENNU_FAILURE_INTERNAL);\n";
  if (!lowering.nodes.empty()) {
    generated += "    goto bennu_failure;\n";
  } else {
    generated += "    (void)bennu_report_failure(&bennu_resources);\n"
                 "    return 1;\n";
  }
  generated += "  }\n";

  for (std::size_t index = 0U; index < lowering.nodes.size(); ++index) {
    const RewriteLoweringNode &node = lowering.nodes[index];
    if (node.kind == RewriteNodeKind::scalar_literal) {
      append_scalar_node(generated, index, node);
    } else if (node.kind == RewriteNodeKind::vector_literal) {
      append_vector_node(generated, index, node);
    } else {
      append_call_node(generated, index, node, lowering);
    }
  }
  for (const std::size_t root : lowering.roots) {
    generated += "  if (!bennu_print_value(&bennu_values[" +
                 std::to_string(root) +
                 "])) { goto bennu_output_failure; }\n";
  }
  if (!lowering.nodes.empty()) {
    generated += "  { size_t bennu_index = 0U;\n"
                 "    for (bennu_index = 0U; bennu_index < ";
    append_c_unsigned(generated, lowering.nodes.size());
    generated +=
        "; ++bennu_index) {\n"
        "      bennu_release(&bennu_resources, &bennu_values[bennu_index]);\n"
        "    }\n"
        "  }\n";
  }
  generated += "  return fflush(stdout) == 0 ? 0 : 1;\n";
  if (!lowering.nodes.empty()) {
    generated += "bennu_failure:\n"
                 "  { size_t bennu_index = 0U;\n"
                 "    for (bennu_index = 0U; bennu_index < ";
    append_c_unsigned(generated, lowering.nodes.size());
    generated += "; ++bennu_index) {\n"
                 "      bennu_release(&bennu_resources, &bennu_values[bennu_index]);\n"
                 "    }\n"
                 "  }\n"
                 "  (void)bennu_report_failure(&bennu_resources);\n"
                 "  return 1;\n";
    if (!lowering.roots.empty()) {
      generated += "bennu_output_failure:\n"
                   "  { size_t bennu_index = 0U;\n"
                   "    for (bennu_index = 0U; bennu_index < ";
      append_c_unsigned(generated, lowering.nodes.size());
      generated +=
          "; ++bennu_index) {\n"
          "      bennu_release(&bennu_resources, &bennu_values[bennu_index]);\n"
          "    }\n"
          "  }\n"
          "  (void)fputs(\"OutputError: stdout failure\\n\", stderr);\n"
          "  return 1;\n";
    }
  }
  generated += "}\n";
  return CEmissionResult{
      true, std::move(generated),
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U})};
}

#ifndef DOCTEST_CONFIG_DISABLE
bool rewrite_program_invariants_hold(const RewriteProgram &program) {
  const auto positions_equal = [](RewritePosition left,
                                  RewritePosition right) {
    return left.offset == right.offset && left.line == right.line &&
           left.column == right.column;
  };
  const auto spans_equal = [&positions_equal](RewriteSpan left,
                                               RewriteSpan right) {
    return positions_equal(left.begin, right.begin) &&
           positions_equal(left.end, right.end);
  };
  const auto span_is_ordered = [&program](RewriteSpan span) {
    return span.begin.offset <= span.end.offset && span.begin.offset >= 1U &&
           span.end.offset <= program.source.size() + 1U;
  };
  if (program.arguments.size() != program.argument_spans.size()) {
    return false;
  }
  std::size_t expected_first_argument = 0U;
  for (const RewriteCall &call : program.calls) {
    if (call.first_argument != expected_first_argument ||
        call.first_argument > program.arguments.size() ||
        call.argument_count > program.arguments.size() - call.first_argument ||
        !span_is_ordered(call.name_span) || !span_is_ordered(call.span)) {
      return false;
    }
    expected_first_argument += call.argument_count;
  }
  if (expected_first_argument != program.arguments.size()) {
    return false;
  }

  std::vector<std::uint8_t> seen_calls(program.calls.size(), std::uint8_t{0U});
  for (std::size_t node_index = 0U; node_index < program.nodes.size();
       ++node_index) {
    const RewriteNode &node = program.nodes[node_index];
    if (!span_is_ordered(node.span)) {
      return false;
    }
    if (node.kind == RewriteNodeKind::vector_literal) {
      if (node.first_element_span > program.vector_element_spans.size() ||
          node.element_count >
              program.vector_element_spans.size() - node.first_element_span) {
        return false;
      }
      const std::size_t payload_size =
          vector_payload_size(program, node.element_type);
      if (node.first_element > payload_size ||
          node.element_count > payload_size - node.first_element) {
        return false;
      }
    }
    if (node.kind != RewriteNodeKind::primitive_call) {
      continue;
    }
    if (node.call_index >= program.calls.size() ||
        seen_calls[node.call_index] != std::uint8_t{0U}) {
      return false;
    }
    seen_calls[node.call_index] = std::uint8_t{1U};
    const RewriteCall &call = program.calls[node.call_index];
    if (!spans_equal(node.span, call.span)) {
      return false;
    }
    for (std::size_t argument = 0U; argument < call.argument_count;
         ++argument) {
      const std::size_t arena_index = call.first_argument + argument;
      const std::size_t argument_node = program.arguments[arena_index];
      if (argument_node >= node_index || argument_node >= program.nodes.size() ||
          !spans_equal(program.argument_spans[arena_index],
                       program.nodes[argument_node].span)) {
        return false;
      }
    }
  }
  for (const std::uint8_t seen : seen_calls) {
    if (seen == std::uint8_t{0U}) {
      return false;
    }
  }
  std::size_t previous_root_offset = 0U;
  for (const std::size_t root : program.roots) {
    if (root >= program.nodes.size() ||
        program.nodes[root].span.begin.offset < previous_root_offset) {
      return false;
    }
    previous_root_offset = program.nodes[root].span.begin.offset;
  }
  return true;
}

bool position_is(RewritePosition position, std::size_t offset,
                 std::size_t line, std::size_t column) {
  return position.offset == offset && position.line == line &&
         position.column == column;
}

bool span_is(RewriteSpan span, std::size_t begin_offset,
             std::size_t begin_line, std::size_t begin_column,
             std::size_t end_offset, std::size_t end_line,
             std::size_t end_column) {
  return position_is(span.begin, begin_offset, begin_line, begin_column) &&
         position_is(span.end, end_offset, end_line, end_column);
}

struct RewriteValidFixture {
  std::string_view name;
  std::string_view source;
  bool accepted;
  std::string_view flat_snapshot;
};

struct RewriteInvalidFixture {
  std::string_view name;
  std::string_view source;
  bool accepted;
  RewriteParseError error;
  RewriteSpan primary;
};

struct RewriteEvaluatorGoldenFixture {
  std::string_view name;
  std::string_view coverage;
  std::string_view source;
  std::string_view formatted;
};

struct RewriteEvaluatorErrorFixture {
  std::string_view name;
  std::string_view coverage;
  std::string_view source;
  ErrorKind error;
  std::optional<std::size_t> argument_position;
  std::optional<std::size_t> element_index;
};

bool error_value_type_equal(ErrorValueType left, ErrorValueType right) {
  return left.container == right.container && left.element == right.element;
}

bool scalar_value_equal(const ScalarValue &left, const ScalarValue &right) {
  if (left.type != right.type) {
    return false;
  }
  if (left.type == ScalarType::boolean) {
    return left.boolean == right.boolean;
  }
  if (left.type == ScalarType::integer) {
    return left.integer == right.integer;
  }
  return std::bit_cast<std::uint64_t>(left.double_precision) ==
         std::bit_cast<std::uint64_t>(right.double_precision);
}

bool value_equal(const Value &left, const Value &right) {
  if (left.container != right.container) {
    return false;
  }
  ScalarType left_type = ScalarType::boolean;
  ScalarType right_type = ScalarType::boolean;
  if (!value_element_type(left, left_type).ok ||
      !value_element_type(right, right_type).ok || left_type != right_type) {
    return false;
  }
  if (left.container == ContainerKind::scalar) {
    return scalar_value_equal(left.scalar, right.scalar);
  }
  std::size_t left_length = 0U;
  std::size_t right_length = 0U;
  if (!value_length(left, left_length).ok ||
      !value_length(right, right_length).ok || left_length != right_length) {
    return false;
  }
  for (std::size_t index = 0U; index < left_length; ++index) {
    const ScalarProjectionResult left_element = project_scalar(left, index);
    const ScalarProjectionResult right_element = project_scalar(right, index);
    if (!left_element.ok || !right_element.ok ||
        !scalar_value_equal(left_element.value, right_element.value)) {
      return false;
    }
  }
  return true;
}

bool type_error_equal(const TypeErrorContext &left,
                      const TypeErrorContext &right) {
  if (left.actual_arguments.size() != right.actual_arguments.size() ||
      left.accepted_signatures.size() != right.accepted_signatures.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.actual_arguments.size(); ++index) {
    if (!error_value_type_equal(left.actual_arguments[index],
                                right.actual_arguments[index])) {
      return false;
    }
  }
  for (std::size_t signature = 0U;
       signature < left.accepted_signatures.size(); ++signature) {
    const TypeErrorSignatureContext &left_signature =
        left.accepted_signatures[signature];
    const TypeErrorSignatureContext &right_signature =
        right.accepted_signatures[signature];
    if (left_signature.parameters.size() !=
            right_signature.parameters.size() ||
        !error_value_type_equal(left_signature.result,
                                right_signature.result)) {
      return false;
    }
    for (std::size_t parameter = 0U;
         parameter < left_signature.parameters.size(); ++parameter) {
      if (!error_value_type_equal(left_signature.parameters[parameter],
                                  right_signature.parameters[parameter])) {
        return false;
      }
    }
  }
  return true;
}

bool domain_error_equal(const DomainErrorContext &left,
                        const DomainErrorContext &right) {
  if (left.reason != right.reason ||
      left.signature.result_type != right.signature.result_type ||
      left.signature.parameter_types != right.signature.parameter_types ||
      left.operands.size() != right.operands.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.operands.size(); ++index) {
    if (!scalar_value_equal(left.operands[index], right.operands[index])) {
      return false;
    }
  }
  return true;
}

bool structured_error_equal(const Error &left, const Error &right) {
  if (left.kind != right.kind ||
      left.primitive.has_value() != right.primitive.has_value() ||
      left.arity.has_value() != right.arity.has_value() ||
      left.type.has_value() != right.type.has_value() ||
      left.argument_position != right.argument_position ||
      left.shape.has_value() != right.shape.has_value() ||
      left.element_index != right.element_index ||
      left.resource.has_value() != right.resource.has_value() ||
      left.domain.has_value() != right.domain.has_value()) {
    return false;
  }
  if (left.primitive.has_value() &&
      (left.primitive->name != right.primitive->name ||
       left.primitive->id != right.primitive->id)) {
    return false;
  }
  if (left.arity.has_value() &&
      (left.arity->supplied != right.arity->supplied ||
       left.arity->accepted != right.arity->accepted)) {
    return false;
  }
  if (left.type.has_value() && !type_error_equal(*left.type, *right.type)) {
    return false;
  }
  if (left.shape.has_value() &&
      (left.shape->expected != right.shape->expected ||
       left.shape->actual != right.shape->actual)) {
    return false;
  }
  if (left.resource.has_value()) {
    const ResourceErrorContext &left_resource = *left.resource;
    const ResourceErrorContext &right_resource = *right.resource;
    if (left_resource.reason != right_resource.reason ||
        left_resource.requested_elements !=
            right_resource.requested_elements ||
        left_resource.requested_bytes != right_resource.requested_bytes ||
        left_resource.profile != right_resource.profile ||
        left_resource.limit_kind != right_resource.limit_kind ||
        left_resource.configured_limit != right_resource.configured_limit ||
        left_resource.usage_before != right_resource.usage_before ||
        left_resource.refused_charge != right_resource.refused_charge) {
      return false;
    }
  }
  return !left.domain.has_value() ||
         domain_error_equal(*left.domain, *right.domain);
}

void append_size(std::string &snapshot, std::size_t value) {
  char buffer[32];
  const auto converted =
      std::to_chars(std::begin(buffer), std::end(buffer), value);
  snapshot.append(buffer, converted.ptr);
}

void append_integer(std::string &snapshot, std::int64_t value) {
  char buffer[32];
  const auto converted =
      std::to_chars(std::begin(buffer), std::end(buffer), value);
  snapshot.append(buffer, converted.ptr);
}

void append_position(std::string &snapshot, RewritePosition position) {
  append_size(snapshot, position.offset);
  snapshot.push_back(':');
  append_size(snapshot, position.line);
  snapshot.push_back(':');
  append_size(snapshot, position.column);
}

void append_span(std::string &snapshot, RewriteSpan span) {
  snapshot.push_back('[');
  append_position(snapshot, span.begin);
  snapshot.push_back(',');
  append_position(snapshot, span.end);
  snapshot.push_back(')');
}

void append_double(std::string &snapshot, double value) {
  if (std::isnan(value)) {
    snapshot.append("nan");
    return;
  }
  if (std::isinf(value)) {
    snapshot.append(std::signbit(value) ? "-inf" : "+inf");
    return;
  }
  snapshot.append("bits:");
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  char buffer[32];
  const auto converted =
      std::to_chars(std::begin(buffer), std::end(buffer), bits, 16);
  snapshot.append(buffer, converted.ptr);
}

std::string_view node_kind_name(RewriteNodeKind kind) {
  if (kind == RewriteNodeKind::scalar_literal) {
    return "scalar_literal";
  }
  if (kind == RewriteNodeKind::vector_literal) {
    return "vector_literal";
  }
  if (kind == RewriteNodeKind::parameter_reference) {
    return "parameter_reference";
  }
  return "primitive_call";
}

std::string_view scalar_type_name(ScalarType type) {
  if (type == ScalarType::boolean) {
    return "boolean";
  }
  if (type == ScalarType::integer) {
    return "integer";
  }
  return "double_precision";
}

std::string_view call_syntax_name(RewriteCallSyntax syntax) {
  return syntax == RewriteCallSyntax::bracketed ? "bracketed" : "prefix";
}

std::string rewrite_flat_snapshot(const RewriteProgram &program) {
  std::string snapshot;
  snapshot.append("roots=[");
  for (std::size_t index = 0U; index < program.roots.size(); ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_size(snapshot, program.roots[index]);
  }
  snapshot.append("];nodes=[");
  for (std::size_t index = 0U; index < program.nodes.size(); ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    const RewriteNode &node = program.nodes[index];
    snapshot.append("{kind=");
    snapshot.append(node_kind_name(node.kind));
    snapshot.append(",span=");
    append_span(snapshot, node.span);
    snapshot.append(",element_type=");
    snapshot.append(scalar_type_name(node.element_type));
    snapshot.append(",boolean=");
    snapshot.push_back(node.boolean ? '1' : '0');
    snapshot.append(",integer=");
    append_integer(snapshot, node.integer);
    snapshot.append(",double_precision=");
    append_double(snapshot, node.double_precision);
    snapshot.append(",first_element=");
    append_size(snapshot, node.first_element);
    snapshot.append(",element_count=");
    append_size(snapshot, node.element_count);
    snapshot.append(",first_element_span=");
    append_size(snapshot, node.first_element_span);
    snapshot.append(",call_index=");
    append_size(snapshot, node.call_index);
    snapshot.push_back('}');
  }
  snapshot.append("];arguments=[");
  for (std::size_t index = 0U; index < program.arguments.size(); ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_size(snapshot, program.arguments[index]);
  }
  snapshot.append("];argument_spans=[");
  for (std::size_t index = 0U; index < program.argument_spans.size(); ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_span(snapshot, program.argument_spans[index]);
  }
  snapshot.append("];calls=[");
  for (std::size_t index = 0U; index < program.calls.size(); ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    const RewriteCall &call = program.calls[index];
    snapshot.append("{syntax=");
    snapshot.append(call_syntax_name(call.syntax));
    snapshot.append(",name=");
    const std::size_t name_begin = call.name_span.begin.offset - 1U;
    const std::size_t name_size =
        call.name_span.end.offset - call.name_span.begin.offset;
    snapshot.append(program.source.data() + name_begin, name_size);
    snapshot.append(",name_span=");
    append_span(snapshot, call.name_span);
    snapshot.append(",opening_delimiter_span=");
    append_span(snapshot, call.opening_delimiter_span);
    snapshot.append(",closing_delimiter_span=");
    append_span(snapshot, call.closing_delimiter_span);
    snapshot.append(",prefix_separator_span=");
    append_span(snapshot, call.prefix_separator_span);
    snapshot.append(",span=");
    append_span(snapshot, call.span);
    snapshot.append(",first_argument=");
    append_size(snapshot, call.first_argument);
    snapshot.append(",argument_count=");
    append_size(snapshot, call.argument_count);
    snapshot.append(",primitive=");
    if (call.primitive.has_value()) {
      append_size(snapshot, static_cast<std::size_t>(*call.primitive));
    } else {
      snapshot.append("none");
    }
    snapshot.push_back('}');
  }
  snapshot.append("];boolean_elements=[");
  for (std::size_t index = 0U; index < program.boolean_elements.size();
       ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_size(snapshot, program.boolean_elements[index]);
  }
  snapshot.append("];integer_elements=[");
  for (std::size_t index = 0U; index < program.integer_elements.size();
       ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_integer(snapshot, program.integer_elements[index]);
  }
  snapshot.append("];double_elements=[");
  for (std::size_t index = 0U; index < program.double_elements.size();
       ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_double(snapshot, program.double_elements[index]);
  }
  snapshot.append("];vector_element_spans=[");
  for (std::size_t index = 0U; index < program.vector_element_spans.size();
       ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_span(snapshot, program.vector_element_spans[index]);
  }
  snapshot.push_back(']');
  return snapshot;
}

std::string_view lowering_cardinality_name(RewriteCardinality cardinality) {
  if (cardinality == RewriteCardinality::scalar) {
    return "scalar";
  }
  if (cardinality == RewriteCardinality::static_vector) {
    return "static_vector";
  }
  return "dynamic_vector";
}

std::string rewrite_lowering_snapshot(const RewriteLoweringProgram &program) {
  std::string snapshot{"roots=["};
  for (std::size_t index = 0U; index < program.roots.size(); ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_size(snapshot, program.roots[index]);
  }
  snapshot.append("];arguments=[");
  for (std::size_t index = 0U; index < program.arguments.size(); ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    append_size(snapshot, program.arguments[index]);
  }
  snapshot.append("];nodes=[");
  for (std::size_t index = 0U; index < program.nodes.size(); ++index) {
    if (index != 0U) {
      snapshot.push_back(',');
    }
    const RewriteLoweringNode &node = program.nodes[index];
    append_size(snapshot, index);
    snapshot.push_back(':');
    snapshot.append(node_kind_name(node.kind));
    snapshot.push_back('/');
    snapshot.append(scalar_type_name(node.element_type));
    snapshot.push_back('/');
    snapshot.append(lowering_cardinality_name(node.cardinality));
    snapshot.push_back('(');
    append_size(snapshot, node.element_count);
    snapshot.append(")/impl=");
    append_size(snapshot, static_cast<std::size_t>(node.implementation));
    snapshot.append("/parameter=");
    if (node.kind == RewriteNodeKind::parameter_reference) {
      append_size(snapshot, node.parameter_index);
    } else {
      snapshot.push_back('-');
    }
    snapshot.append("/arguments=");
    append_size(snapshot, node.first_argument);
    snapshot.push_back('+');
    append_size(snapshot, node.argument_count);
    snapshot.append("/shape_check=");
    snapshot.push_back(node.runtime_shape_check ? '1' : '0');
    snapshot.append("/span=");
    append_size(snapshot, node.source_span.begin.offset);
    snapshot.push_back('-');
    append_size(snapshot, node.source_span.end.offset);
  }
  snapshot.push_back(']');
  return snapshot;
}

#include "../tests/fixtures/rewrite_conformance_fixture.inc"
#include "../tests/fixtures/rewrite_evaluator_conformance_fixture.inc"
#endif

TEST_CASE("rewrite tokenizer uses generic categories and one-based byte spans") {
  const RewriteTokens tokens =
      tokenize_rewrite("true -9223372036854775808\r\nDouble()");

  REQUIRE(tokens.tokens.size() == 7U);
  CHECK(tokens.tokens[0].kind == RewriteTokenKind::bool_literal);
  CHECK(tokens.tokens[0].boolean);
  CHECK(span_is(tokens.tokens[0].span, 1U, 1U, 1U, 5U, 1U, 5U));
  CHECK(tokens.tokens[1].kind == RewriteTokenKind::horizontal_space);
  CHECK(tokens.tokens[2].kind == RewriteTokenKind::int_literal);
  CHECK(tokens.tokens[2].integer == std::numeric_limits<std::int64_t>::min());
  CHECK(tokens.tokens[3].kind == RewriteTokenKind::line_terminator);
  CHECK(span_is(tokens.tokens[3].span, 26U, 1U, 26U, 28U, 2U, 1U));
  CHECK(tokens.tokens[4].kind == RewriteTokenKind::double_type);
  CHECK(tokens.tokens[5].kind == RewriteTokenKind::left_parenthesis);
  CHECK(tokens.tokens[6].kind == RewriteTokenKind::right_parenthesis);
  CHECK(position_is(tokens.end, 36U, 2U, 9U));
}

TEST_CASE("rewrite tokenizer enforces canonical and complete numeric literals") {
  struct NumericCase {
    std::string_view source;
    RewriteTokenKind kind;
    RewriteLiteralError error;
  };
  const NumericCase cases[] = {
      {"9223372036854775807", RewriteTokenKind::int_literal,
       RewriteLiteralError::none},
      {"-9223372036854775808", RewriteTokenKind::int_literal,
       RewriteLiteralError::none},
      {"9223372036854775808", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::range},
      {"-9223372036854775809", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::range},
      {"-0", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"+1", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"00", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"01.0", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {".5", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"1.", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"1e", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"1e+", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"1_000", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"0x10", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"1.0f", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::malformed},
      {"1e9999", RewriteTokenKind::malformed_literal,
       RewriteLiteralError::range},
  };
  for (const NumericCase &numeric_case : cases) {
    INFO(std::string(numeric_case.source));
    const RewriteTokens tokens = tokenize_rewrite(numeric_case.source);
    REQUIRE(tokens.tokens.size() == 1U);
    CHECK(tokens.tokens[0].kind == numeric_case.kind);
    CHECK(tokens.tokens[0].literal_error == numeric_case.error);
    CHECK(tokens.tokens[0].span.begin.offset == 1U);
    CHECK(tokens.tokens[0].span.end.offset == numeric_case.source.size() + 1U);
  }
}

TEST_CASE("rewrite tokenizer preserves binary64 boundaries and signed zero") {
  const RewriteTokens tokens = tokenize_rewrite(
      "0.0 -0.0 4.9406564584124654e-324 "
      "1.7976931348623157e308 inf -inf nan 1e-9999 -1e-9999");
  std::vector<double> values;
  for (const RewriteToken &token : tokens.tokens) {
    if (token.kind == RewriteTokenKind::double_literal) {
      values.push_back(token.double_precision);
    }
  }
  REQUIRE(values.size() == 9U);
  CHECK(values[0] == 0.0);
  CHECK_FALSE(std::signbit(values[0]));
  CHECK(values[1] == 0.0);
  CHECK(std::signbit(values[1]));
  CHECK(values[2] == std::numeric_limits<double>::denorm_min());
  CHECK(values[3] == std::numeric_limits<double>::max());
  CHECK(values[4] == std::numeric_limits<double>::infinity());
  CHECK(values[5] == -std::numeric_limits<double>::infinity());
  CHECK(std::isnan(values[6]));
  CHECK(values[7] == 0.0);
  CHECK_FALSE(std::signbit(values[7]));
  CHECK(values[8] == 0.0);
  CHECK(std::signbit(values[8]));
}

TEST_CASE("rewrite tokenizer distinguishes decimal overflow from underflow") {
  const std::string overflow = std::string(310U, '9') + ".0";
  const RewriteTokens overflow_tokens = tokenize_rewrite(overflow);
  REQUIRE(overflow_tokens.tokens.size() == 1U);
  CHECK(overflow_tokens.tokens[0].kind ==
        RewriteTokenKind::malformed_literal);
  CHECK(overflow_tokens.tokens[0].literal_error ==
        RewriteLiteralError::range);

  const std::string_view invalid_specials[] = {
      "+inf", "-nan", "Inf", "NAN", "nan(payload)"};
  for (const std::string_view source : invalid_specials) {
    INFO(std::string(source));
    const RewriteParseResult parsed = parse_rewrite(source);
    CHECK_FALSE(parsed.ok);
  }
}

#if defined(__APPLE__) || defined(BENNU_REWRITE_STRTOD_L)
TEST_CASE("rewrite strtod fallback uses nearest rounding and restores the mode") {
  const int original_rounding = std::fegetround();
  const int set_result = std::fesetround(FE_UPWARD);
  const RewriteTokens tokens = tokenize_rewrite("0.1");
  const int observed_rounding = std::fegetround();
  const int restore_result = original_rounding == -1
                                 ? 0
                                 : std::fesetround(original_rounding);

  REQUIRE(original_rounding != -1);
  REQUIRE(set_result == 0);
  REQUIRE(tokens.tokens.size() == 1U);
  CHECK(tokens.tokens[0].kind == RewriteTokenKind::double_literal);
  CHECK(tokens.tokens[0].double_precision == 0x1.999999999999ap-4);
  CHECK(observed_rounding == FE_UPWARD);
  CHECK(restore_result == 0);
}
#endif

TEST_CASE("rewrite parser builds postorder generic calls and contiguous arenas") {
  const RewriteParseResult parsed = parse_rewrite(
      "true\nInt()\nadd[1 inc 2 3.0]\ninc inc 5");
  if (!parsed.ok) {
    CHECK(parsed.ok);
    return;
  }

  REQUIRE(parsed.program.roots.size() == 4U);
  REQUIRE(parsed.program.nodes.size() == 10U);
  REQUIRE(parsed.program.calls.size() == 4U);
  CHECK(parsed.program.roots[0] == 0U);
  CHECK(parsed.program.roots[1] == 1U);
  CHECK(parsed.program.roots[2] == 6U);
  CHECK(parsed.program.roots[3] == 9U);

  const RewriteNode &empty = parsed.program.nodes[1];
  CHECK(empty.kind == RewriteNodeKind::vector_literal);
  CHECK(empty.element_type == ScalarType::integer);
  CHECK(empty.element_count == 0U);
  CHECK(span_is(empty.span, 6U, 2U, 1U, 11U, 2U, 6U));

  const RewriteNode &outer_bracket = parsed.program.nodes[6];
  REQUIRE(outer_bracket.kind == RewriteNodeKind::primitive_call);
  const RewriteCall &add = parsed.program.calls[outer_bracket.call_index];
  CHECK(add.syntax == RewriteCallSyntax::bracketed);
  CHECK(add.argument_count == 3U);
  CHECK(add.first_argument == 1U);
  CHECK(parsed.program.arguments[1] == 2U);
  CHECK(parsed.program.arguments[2] == 4U);
  CHECK(parsed.program.arguments[3] == 5U);
  CHECK(span_is(add.name_span, 12U, 3U, 1U, 15U, 3U, 4U));
  CHECK(span_is(add.opening_delimiter_span, 15U, 3U, 4U, 16U, 3U, 5U));
  CHECK(span_is(add.closing_delimiter_span, 27U, 3U, 16U, 28U, 3U, 17U));
  CHECK(span_is(add.span, 12U, 3U, 1U, 28U, 3U, 17U));
  CHECK(span_is(parsed.program.argument_spans[1], 16U, 3U, 5U, 17U, 3U,
                6U));
  CHECK(span_is(parsed.program.argument_spans[2], 18U, 3U, 7U, 23U, 3U,
                12U));
  CHECK(span_is(parsed.program.argument_spans[3], 24U, 3U, 13U, 27U, 3U,
                16U));

  const RewriteCall &inner_prefix =
      parsed.program.calls[parsed.program.nodes[8].call_index];
  const RewriteCall &outer_prefix =
      parsed.program.calls[parsed.program.nodes[9].call_index];
  CHECK(inner_prefix.argument_count == 1U);
  CHECK(outer_prefix.argument_count == 1U);
  CHECK(parsed.program.arguments[inner_prefix.first_argument] == 7U);
  CHECK(parsed.program.arguments[outer_prefix.first_argument] == 8U);
  CHECK(span_is(outer_prefix.prefix_separator_span, 32U, 4U, 4U, 33U, 4U,
                5U));
  CHECK(span_is(outer_prefix.span, 29U, 4U, 1U, 38U, 4U, 10U));

  for (std::size_t node_index = 0U;
       node_index < parsed.program.nodes.size(); ++node_index) {
    const RewriteNode &node = parsed.program.nodes[node_index];
    if (node.kind == RewriteNodeKind::primitive_call) {
      const RewriteCall &call = parsed.program.calls[node.call_index];
      CHECK(call.first_argument + call.argument_count <=
            parsed.program.arguments.size());
      for (std::size_t argument = 0U; argument < call.argument_count;
           ++argument) {
        CHECK(parsed.program.arguments[call.first_argument + argument] <
              node_index);
      }
    }
  }
}

#ifndef DOCTEST_CONFIG_DISABLE
TEST_CASE("rewrite parser matches normative flat conformance fixtures") {
  for (const RewriteValidFixture &fixture : rewrite_valid_fixtures) {
    INFO(std::string(fixture.name));
    const RewriteParseResult parsed = parse_rewrite(fixture.source);
    REQUIRE(parsed.ok == fixture.accepted);
    REQUIRE(parsed.program.source == fixture.source);
    CHECK(rewrite_flat_snapshot(parsed.program) == fixture.flat_snapshot);
  }

  for (const RewriteInvalidFixture &fixture : rewrite_invalid_fixtures) {
    INFO(std::string(fixture.name));
    const RewriteParseResult parsed = parse_rewrite(fixture.source);
    REQUIRE(parsed.ok == fixture.accepted);
    CHECK(parsed.diagnostic.error == fixture.error);
    CHECK(position_is(parsed.diagnostic.primary.begin,
                      fixture.primary.begin.offset,
                      fixture.primary.begin.line,
                      fixture.primary.begin.column));
    CHECK(position_is(parsed.diagnostic.primary.end,
                      fixture.primary.end.offset,
                      fixture.primary.end.line,
                      fixture.primary.end.column));
  }
}
#endif

TEST_CASE("rewrite parser retains typed homogeneous vector payloads and spans") {
  const RewriteParseResult parsed =
      parse_rewrite("Bool()\nInt()\nDouble()\n(false true)\n(1 2)\n(1.0 -0.0)");
  if (!parsed.ok) {
    CHECK(parsed.ok);
    return;
  }
  REQUIRE(parsed.program.roots.size() == 6U);
  CHECK(parsed.program.nodes[parsed.program.roots[0]].element_type ==
        ScalarType::boolean);
  CHECK(parsed.program.nodes[parsed.program.roots[1]].element_type ==
        ScalarType::integer);
  CHECK(parsed.program.nodes[parsed.program.roots[2]].element_type ==
        ScalarType::double_precision);
  CHECK(parsed.program.boolean_elements.size() == 2U);
  CHECK(parsed.program.boolean_elements[0] == std::uint8_t{0U});
  CHECK(parsed.program.boolean_elements[1] == std::uint8_t{1U});
  CHECK(parsed.program.integer_elements.size() == 2U);
  CHECK(parsed.program.integer_elements[0] == 1);
  CHECK(parsed.program.integer_elements[1] == 2);
  CHECK(parsed.program.double_elements.size() == 2U);
  CHECK(parsed.program.double_elements[0] == 1.0);
  CHECK(std::signbit(parsed.program.double_elements[1]));
  REQUIRE(parsed.program.vector_element_spans.size() == 6U);
  CHECK(span_is(parsed.program.vector_element_spans[0], 24U, 4U, 2U, 29U,
                4U, 7U));
  CHECK(span_is(parsed.program.vector_element_spans[1], 30U, 4U, 8U, 34U,
                4U, 12U));
  const RewriteNode &double_vector =
      parsed.program.nodes[parsed.program.roots[5]];
  CHECK(span_is(double_vector.span, 42U, 6U, 1U, 52U, 6U, 11U));
}

TEST_CASE("rewrite parser applies logical-record and line-ending rules") {
  const std::string_view valid_programs[] = {
      "", " \t", "\n\n", " \t\r\n\r\n", "true", "true\n",
      "true\r\n\r\nfalse\r\n", "add[\r\n 1\r\n 2\r\n]\r\n"};
  for (const std::string_view source : valid_programs) {
    INFO(std::string(source));
    CHECK(parse_rewrite(source).ok);
  }

  const RewriteParseResult crlf = parse_rewrite("true\r\nfalse");
  REQUIRE(crlf.ok);
  REQUIRE(crlf.program.roots.size() == 2U);
  CHECK(position_is(crlf.program.nodes[crlf.program.roots[1]].span.begin, 7U,
                    2U, 1U));

  const RewriteParseResult bare_cr = parse_rewrite("true\rfalse");
  CHECK_FALSE(bare_cr.ok);
  CHECK(bare_cr.diagnostic.error == RewriteParseError::invalid_byte);
  CHECK(span_is(bare_cr.diagnostic.primary, 5U, 1U, 5U, 6U, 1U, 6U));
}

TEST_CASE("rewrite parser rejects normative invalid syntax at exact spans") {
  struct InvalidCase {
    std::string_view source;
    RewriteParseError error;
    std::size_t begin;
    std::size_t end;
  };
  const InvalidCase cases[] = {
      {"False", RewriteParseError::invalid_byte, 1U, 6U},
      {"-0", RewriteParseError::malformed_literal, 1U, 3U},
      {"1.", RewriteParseError::malformed_literal, 1U, 3U},
      {"(1 2.0)", RewriteParseError::heterogeneous_vector, 4U, 7U},
      {"()", RewriteParseError::bare_empty_vector, 1U, 3U},
      {"Vector<Int>()", RewriteParseError::invalid_byte, 1U, 7U},
      {"((1 2))", RewriteParseError::invalid_vector_element, 2U, 3U},
      {"(inc 1)", RewriteParseError::invalid_vector_element, 2U, 5U},
      {"add [1 2]", RewriteParseError::whitespace_before_bracket, 4U, 5U},
      {"add[1, 2]", RewriteParseError::invalid_byte, 6U, 7U},
      {"add[1 2", RewriteParseError::missing_delimiter, 8U, 8U},
      {"add[(1 2] 3]", RewriteParseError::mismatched_delimiter, 9U, 10U},
      {"add[iota[3]10]", RewriteParseError::missing_separator, 12U, 14U},
      {"add 1 2", RewriteParseError::trailing_input, 7U, 8U},
      {"inc 1 inc 2", RewriteParseError::trailing_input, 7U, 10U},
      {"add[1(2)]", RewriteParseError::missing_separator, 6U, 7U},
      {"add[1 2)", RewriteParseError::mismatched_delimiter, 8U, 9U},
      {"(1 2", RewriteParseError::missing_delimiter, 5U, 5U},
      {"]", RewriteParseError::mismatched_delimiter, 1U, 2U},
      {"inc5", RewriteParseError::primitive_requires_application, 1U, 5U},
      {"inc\n5", RewriteParseError::primitive_requires_application, 1U, 4U},
      {"Int( )", RewriteParseError::invalid_vector_element, 1U, 4U},
      {"(1, 2)", RewriteParseError::invalid_byte, 3U, 4U},
      {"true false", RewriteParseError::trailing_input, 6U, 11U},
      {"1e9999", RewriteParseError::literal_range, 1U, 7U},
  };
  for (const InvalidCase &invalid_case : cases) {
    INFO(std::string(invalid_case.source));
    const RewriteParseResult parsed = parse_rewrite(invalid_case.source);
    REQUIRE_FALSE(parsed.ok);
    CHECK(parsed.diagnostic.error == invalid_case.error);
    CHECK(parsed.diagnostic.primary.begin.offset == invalid_case.begin);
    CHECK(parsed.diagnostic.primary.end.offset == invalid_case.end);
  }
}

TEST_CASE("rewrite syntax diagnostics retain exact positions and context") {
  const RewriteParseResult missing_separator =
      parse_rewrite("add[iota[3]10]");
  REQUIRE_FALSE(missing_separator.ok);
  CHECK(missing_separator.diagnostic.error ==
        RewriteParseError::missing_separator);
  CHECK(span_is(missing_separator.diagnostic.primary, 12U, 1U, 12U, 14U,
                1U, 14U));
  CHECK(span_is(missing_separator.diagnostic.context, 1U, 1U, 1U, 15U,
                1U, 15U));
  CHECK(span_is(missing_separator.diagnostic.related, 4U, 1U, 4U, 5U, 1U,
                5U));

  const RewriteParseResult missing_close =
      parse_rewrite("\r\nadd[\r\n 1\r\n 2");
  REQUIRE_FALSE(missing_close.ok);
  CHECK(missing_close.diagnostic.error == RewriteParseError::missing_delimiter);
  CHECK(span_is(missing_close.diagnostic.primary, 15U, 4U, 3U, 15U, 4U,
                3U));
  CHECK(span_is(missing_close.diagnostic.related, 6U, 2U, 4U, 7U, 2U,
                5U));

  const RewriteParseResult mismatch = parse_rewrite("add[\r\n(1]");
  REQUIRE_FALSE(mismatch.ok);
  CHECK(mismatch.diagnostic.error ==
        RewriteParseError::mismatched_delimiter);
  CHECK(span_is(mismatch.diagnostic.primary, 9U, 2U, 3U, 10U, 2U, 4U));

  const RewriteParseResult trailing = parse_rewrite("true\r\nfalse true");
  REQUIRE_FALSE(trailing.ok);
  CHECK(trailing.diagnostic.error == RewriteParseError::trailing_input);
  CHECK(span_is(trailing.diagnostic.primary, 13U, 2U, 7U, 17U, 2U, 11U));

  const RewriteParseResult nested_vector = parse_rewrite("((1 2))");
  REQUIRE_FALSE(nested_vector.ok);
  CHECK(nested_vector.diagnostic.error ==
        RewriteParseError::invalid_vector_element);
  CHECK(span_is(nested_vector.diagnostic.context, 1U, 1U, 1U, 8U, 1U, 8U));
}

TEST_CASE("rewrite primitive resolution is separate and uses stable metadata") {
  RewriteParseResult parsed = parse_rewrite(
      "inc[1]\nadd[1 2]\nequals[true false]\nnot true\niota[3]");
  if (!parsed.ok) {
    CHECK(parsed.ok);
    return;
  }
  REQUIRE(parsed.program.calls.size() == 5U);
  for (const RewriteCall &call : parsed.program.calls) {
    CHECK_FALSE(call.primitive.has_value());
  }

  const RewriteResolutionResult resolved =
      resolve_rewrite_primitives(parsed.program);
  CHECK(resolved.ok);
  if (!resolved.ok) {
    return;
  }
  CHECK(parsed.program.calls[0].primitive == PrimitiveId::inc);
  CHECK(parsed.program.calls[1].primitive == PrimitiveId::add);
  CHECK(parsed.program.calls[2].primitive == PrimitiveId::equals);
  CHECK(parsed.program.calls[3].primitive == PrimitiveId::logical_not);
  CHECK(parsed.program.calls[4].primitive == PrimitiveId::iota);
  const PrimitiveDescriptor *add = find_primitive(PrimitiveId::add);
  REQUIRE(add != nullptr);
  CHECK(add->lifting == LiftingMode::elementwise);
  CHECK(add->signatures[0].parameter_count == 2U);
}

TEST_CASE("typed lowering is value independent and retains dynamic shape data") {
  RewriteParseResult parsed =
      parse_rewrite("add[(1 2) iota[1]]\nadd[1 2.0]");
  REQUIRE(parsed.ok);
  REQUIRE(resolve_rewrite_primitives(parsed.program).ok);
  REQUIRE(parsed.program.nodes.size() == 7U);
  const RewriteLoweringResult literal_lowering =
      lower_rewrite_program(parsed.program);
  REQUIRE(literal_lowering.ok);

  // Issue #46 will make the parser produce these nodes from declarations.  The
  // lowering seam already accepts the declared scalar type and slot index; it
  // does not need a bound Value.
  parsed.program.nodes[1].kind = RewriteNodeKind::parameter_reference;
  parsed.program.nodes[1].element_type = ScalarType::integer;
  parsed.program.nodes[1].first_element = 42U;
  parsed.program.nodes[4].kind = RewriteNodeKind::parameter_reference;
  parsed.program.nodes[4].element_type = ScalarType::integer;
  parsed.program.nodes[4].first_element = 7U;
  parsed.program.nodes[5].kind = RewriteNodeKind::parameter_reference;
  parsed.program.nodes[5].element_type = ScalarType::double_precision;
  parsed.program.nodes[5].first_element = 8U;

  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  REQUIRE(lowered.ok);
  REQUIRE(lowered.program.nodes.size() == parsed.program.nodes.size());
  CHECK(rewrite_lowering_snapshot(lowered.program) ==
        "roots=[3,6];arguments=[1,0,2,4,5];nodes=["
        "0:vector_literal/integer/static_vector(2)/impl=0/parameter=-/"
        "arguments=0+0/shape_check=0/span=5-10,"
        "1:parameter_reference/integer/scalar(1)/impl=0/parameter=42/"
        "arguments=0+0/shape_check=0/span=16-17,"
        "2:primitive_call/integer/dynamic_vector(0)/impl=9/parameter=-/"
        "arguments=0+1/shape_check=0/span=11-18,"
        "3:primitive_call/integer/dynamic_vector(2)/impl=3/parameter=-/"
        "arguments=1+2/shape_check=1/span=1-19,"
        "4:parameter_reference/integer/scalar(1)/impl=0/parameter=7/"
        "arguments=0+0/shape_check=0/span=24-25,"
        "5:parameter_reference/double_precision/scalar(1)/impl=0/parameter=8/"
        "arguments=0+0/shape_check=0/span=26-29,"
        "6:primitive_call/double_precision/scalar(1)/impl=4/parameter=-/"
        "arguments=3+2/shape_check=0/span=20-30]");

  const RewriteLoweringNode &integer_parameter = lowered.program.nodes[1];
  CHECK(integer_parameter.kind == RewriteNodeKind::parameter_reference);
  CHECK(integer_parameter.cardinality == RewriteCardinality::scalar);
  CHECK(integer_parameter.element_type == ScalarType::integer);
  CHECK(integer_parameter.parameter_index == 42U);
  CHECK(integer_parameter.source_span.begin.offset ==
        parsed.program.nodes[1].span.begin.offset);
  CHECK(integer_parameter.source_span.end.offset ==
        parsed.program.nodes[1].span.end.offset);
  CHECK(integer_parameter.source_location.offset ==
        parsed.program.nodes[1].span.begin.offset);

  const RewriteLoweringNode &iota = lowered.program.nodes[2];
  CHECK(iota.implementation == PrimitiveImplementation::iota_integer);
  CHECK(iota.element_type == ScalarType::integer);
  CHECK(iota.cardinality == RewriteCardinality::dynamic_vector);

  const RewriteLoweringNode &add = lowered.program.nodes[3];
  CHECK(add.implementation == PrimitiveImplementation::add_integer);
  CHECK(add.cardinality == RewriteCardinality::dynamic_vector);
  CHECK(add.runtime_shape_check);
  CHECK(add.element_count == 2U);

  const RewriteLoweringNode &promoted_add = lowered.program.nodes[6];
  CHECK(promoted_add.implementation == PrimitiveImplementation::add_double);
  CHECK(promoted_add.implementation ==
        literal_lowering.program.nodes[6].implementation);
  CHECK(promoted_add.element_type == ScalarType::double_precision);
  CHECK(promoted_add.cardinality == RewriteCardinality::scalar);
  CHECK(lowered.program.nodes[4].element_type == ScalarType::integer);
  CHECK(lowered.program.nodes[4].parameter_index == 7U);
  CHECK(lowered.program.nodes[5].element_type == ScalarType::double_precision);
  CHECK(lowered.program.nodes[5].parameter_index == 8U);
}

TEST_CASE("typed lowering applies whole-program phase precedence") {
  RewriteParseResult parsed = parse_rewrite("add[true 1]\ninc[1 2]");
  REQUIRE(parsed.ok);
  REQUIRE(resolve_rewrite_primitives(parsed.program).ok);

  const RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  REQUIRE_FALSE(lowered.ok);
  CHECK(lowered.diagnostic.error.kind == ErrorKind::arity_error);
  CHECK(lowered.diagnostic.error.primitive.has_value());
  if (lowered.diagnostic.error.primitive.has_value()) {
    CHECK(lowered.diagnostic.error.primitive->id == PrimitiveId::inc);
  }
  CHECK(lowered.diagnostic.primary.begin.line == 2U);
}

TEST_CASE("typed lowering checks type errors before static shape errors across roots") {
  RewriteParseResult parsed =
      parse_rewrite("add[(1 2) (3)]\nadd[true 1]");
  REQUIRE(parsed.ok);
  REQUIRE(resolve_rewrite_primitives(parsed.program).ok);

  const RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  REQUIRE_FALSE(lowered.ok);
  CHECK(lowered.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(lowered.diagnostic.error.argument_position == 1U);
  REQUIRE(lowered.diagnostic.error.primitive.has_value());
  CHECK(lowered.diagnostic.error.primitive->id == PrimitiveId::add);
  CHECK(lowered.diagnostic.primary.begin.line == 2U);
  CHECK(lowered.diagnostic.primary.begin.column == 5U);
}

TEST_CASE("rewrite parser preserves explicit arity before metadata validation") {
  RewriteParseResult parsed =
      parse_rewrite("add[]\nadd 1\nfuture_name[1 2 3]");
  if (!parsed.ok) {
    CHECK(parsed.ok);
    return;
  }
  REQUIRE(parsed.program.calls.size() == 3U);
  CHECK(parsed.program.calls[0].argument_count == 0U);
  CHECK(parsed.program.calls[1].argument_count == 1U);
  CHECK(parsed.program.calls[2].argument_count == 3U);
  CHECK_FALSE(parsed.program.calls[0].primitive.has_value());
  CHECK_FALSE(parsed.program.calls[1].primitive.has_value());

  const RewriteResolutionResult resolution =
      resolve_rewrite_primitives(parsed.program);
  CHECK_FALSE(resolution.ok);
  CHECK(resolution.diagnostic.error == RewriteParseError::unknown_primitive);
  CHECK(span_is(resolution.diagnostic.primary, 13U, 3U, 1U, 24U, 3U, 12U));
  CHECK_FALSE(parsed.program.calls[0].primitive.has_value());
  CHECK_FALSE(parsed.program.calls[1].primitive.has_value());
}

TEST_CASE("rewrite flat program satisfies all arena and postorder invariants") {
  const RewriteParseResult parsed = parse_rewrite(
      "true\nadd[iota[3] inc 4 future[5 6 7]]\n(false true)\nInt()");
  REQUIRE(parsed.ok);
  CHECK(rewrite_program_invariants_hold(parsed.program));
}

TEST_CASE("rewrite parser handles deep valid and invalid input iteratively") {
  constexpr std::size_t depth = 4000U;
  std::string prefix;
  prefix.reserve(depth * 4U + 1U);
  for (std::size_t index = 0U; index < depth; ++index) {
    prefix += "inc ";
  }
  prefix += '1';
  const RewriteParseResult prefix_parsed = parse_rewrite(prefix);
  REQUIRE(prefix_parsed.ok);
  CHECK(prefix_parsed.program.nodes.size() == depth + 1U);
  CHECK(rewrite_program_invariants_hold(prefix_parsed.program));

  std::string brackets;
  brackets.reserve(depth * 5U + 1U);
  for (std::size_t index = 0U; index < depth; ++index) {
    brackets += "inc[";
  }
  brackets += '1';
  for (std::size_t index = 0U; index < depth; ++index) {
    brackets += ']';
  }
  const RewriteParseResult bracket_parsed = parse_rewrite(brackets);
  REQUIRE(bracket_parsed.ok);
  CHECK(bracket_parsed.program.nodes.size() == depth + 1U);
  CHECK(rewrite_program_invariants_hold(bracket_parsed.program));

  brackets.pop_back();
  const RewriteParseResult missing = parse_rewrite(brackets);
  CHECK_FALSE(missing.ok);
  CHECK(missing.diagnostic.error == RewriteParseError::missing_delimiter);
  CHECK(missing.diagnostic.primary.begin.offset == brackets.size() + 1U);
  CHECK(missing.diagnostic.primary.begin.offset ==
        missing.diagnostic.primary.end.offset);
}

TEST_CASE("rewrite parser is deterministic over a fixed adversarial corpus") {
  constexpr char alphabet[] = {'a', 'Z', '0', '9', '-', '+', '.', '_', '[',
                               ']', '(', ')', ' ', '\t', '\n', '\r', ',',
                               ';', '{', '}', '\0', static_cast<char>(0xC3)};
  std::uint32_t state = 0x31B3A55DU;
  for (std::size_t case_index = 0U; case_index < 256U; ++case_index) {
    state = state * 1664525U + 1013904223U;
    const std::size_t length = static_cast<std::size_t>(state % 96U);
    std::string source;
    source.reserve(length);
    for (std::size_t byte = 0U; byte < length; ++byte) {
      state = state * 1664525U + 1013904223U;
      const std::size_t alphabet_index =
          static_cast<std::size_t>(state) % std::size(alphabet);
      source.push_back(alphabet[alphabet_index]);
    }
    const RewriteParseResult first = parse_rewrite(source);
    const RewriteParseResult second = parse_rewrite(source);
    CAPTURE(case_index);
    CHECK(first.ok == second.ok);
    CHECK(first.diagnostic.error == second.diagnostic.error);
    CHECK(first.diagnostic.primary.begin.offset ==
          second.diagnostic.primary.begin.offset);
    CHECK(first.diagnostic.primary.end.offset ==
          second.diagnostic.primary.end.offset);
    if (first.ok) {
      CHECK(rewrite_program_invariants_hold(first.program));
      CHECK(rewrite_program_invariants_hold(second.program));
    }
  }
}

TEST_CASE("rewrite evaluator returns formatted scalar roots in source order") {
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult evaluated =
      evaluate_rewrite_source("true\n42\n-0.0", creation);

  REQUIRE(evaluated.ok);
  REQUIRE(evaluated.values.size() == 3U);
  REQUIRE(evaluated.formatted.size() == 3U);
  CHECK(evaluated.formatted[0] == "true");
  CHECK(evaluated.formatted[1] == "42");
  CHECK(evaluated.formatted[2] == "-0.0");
  CHECK(evaluated.resources.live_evaluation_bytes == 0U);
  CHECK(evaluated.resources.work_units == 0U);
  release_rewrite_evaluation_result(evaluated);
}

TEST_CASE("typed runtime shape checks honor static anchors and first mismatch order") {
  struct ShapeCase {
    std::string_view source;
    std::size_t argument_position;
    std::size_t expected_count;
    std::size_t actual_count;
    std::size_t column;
  };
  const std::array<ShapeCase, 3> cases{{
      {"add[iota[3] (1 2)]", 1U, 2U, 3U, 5U},
      {"add[(1 2) iota[3]]", 2U, 2U, 3U, 11U},
      {"add[iota[2] iota[3]]", 2U, 2U, 3U, 13U},
  }};
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};

  for (const ShapeCase &shape_case : cases) {
    CAPTURE(shape_case.source);
    RewriteEvaluationResult evaluated =
        evaluate_rewrite_source(shape_case.source, creation);
    REQUIRE_FALSE(evaluated.ok);
    CHECK(evaluated.diagnostic.error.kind == ErrorKind::shape_mismatch);
    CHECK(evaluated.diagnostic.error.argument_position ==
          shape_case.argument_position);
    REQUIRE(evaluated.diagnostic.error.shape.has_value());
    CHECK(evaluated.diagnostic.error.shape->expected ==
          std::vector<std::size_t>{shape_case.expected_count});
    CHECK(evaluated.diagnostic.error.shape->actual ==
          std::vector<std::size_t>{shape_case.actual_count});
    CHECK(evaluated.diagnostic.primary.begin.line == 1U);
    CHECK(evaluated.diagnostic.primary.begin.column == shape_case.column);
    CHECK(evaluated.diagnostic.error.location.line == 1U);
    CHECK(evaluated.diagnostic.error.location.column == shape_case.column);
    CHECK(evaluated.scalar_kernel_invocations == 0U);
    CHECK(evaluated.values.empty());
    CHECK(evaluated.formatted.empty());
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
  }
}

TEST_CASE("rewrite evaluator validates every complete execution profile early") {
  struct MalformedProfileCase {
    RewriteEvaluationCreationData creation;
    std::string_view source;
  };
  const ResourceLimits no_limits{
      std::nullopt, std::nullopt, std::nullopt};
  const ResourceLimits vector_limit{8U, std::nullopt, std::nullopt};
  const std::array<MalformedProfileCase, 6> cases{{
      {{ExecutionProfile::bounded_v1, no_limits,
        AllocationFailureInjection{std::nullopt}},
       ""},
      {{ExecutionProfile::bounded_v1, no_limits,
        AllocationFailureInjection{std::nullopt}},
       "42"},
      {{ExecutionProfile::trusted_local_v1, vector_limit,
        AllocationFailureInjection{std::nullopt}},
       ""},
      {{ExecutionProfile::trusted_local_v1, vector_limit,
        AllocationFailureInjection{std::nullopt}},
       "42"},
      {{static_cast<ExecutionProfile>(999), no_limits,
        AllocationFailureInjection{std::nullopt}},
       ""},
      {{static_cast<ExecutionProfile>(999), no_limits,
        AllocationFailureInjection{std::nullopt}},
       "42"},
  }};

  for (const MalformedProfileCase &profile_case : cases) {
    RewriteEvaluationResult evaluated = evaluate_rewrite_source(
        profile_case.source, profile_case.creation);
    REQUIRE_FALSE(evaluated.ok);
    CHECK(evaluated.diagnostic.stage ==
          RewriteEvaluationStage::resource_admission);
    CHECK(evaluated.diagnostic.error.kind ==
          ErrorKind::invalid_execution_profile);
    REQUIRE(evaluated.diagnostic.error.primitive.has_value());
    CHECK(evaluated.diagnostic.error.primitive->name == "rewrite-evaluator");
    CHECK_FALSE(evaluated.diagnostic.error.resource.has_value());
    CHECK(evaluated.diagnostic.error.location.offset == 1U);
    CHECK(evaluated.diagnostic.error.location.line == 1U);
    CHECK(evaluated.diagnostic.error.location.column == 1U);
    CHECK(evaluated.values.empty());
    CHECK(evaluated.formatted.empty());
    CHECK(evaluated.scalar_kernel_invocations == 0U);
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
    CHECK(evaluated.resources.work_units == 0U);
    CHECK(evaluated.resources.reservation_ordinal == 0U);
  }
}

TEST_CASE("rewrite evaluator constructs accounted typed vector literals") {
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult evaluated = evaluate_rewrite_source(
      "(false true)\n(1 2)\n(1.0 -0.0)\nBool()\nInt()\nDouble()",
      creation);

  REQUIRE(evaluated.ok);
  if (!evaluated.ok) {
    return;
  }
  REQUIRE(evaluated.formatted.size() == 6U);
  if (evaluated.formatted.size() != 6U) {
    release_rewrite_evaluation_result(evaluated);
    return;
  }
  CHECK(evaluated.formatted[0] == "(false true)");
  CHECK(evaluated.formatted[1] == "(1 2)");
  CHECK(evaluated.formatted[2] == "(1.0 -0.0)");
  CHECK(evaluated.formatted[3] == "()");
  CHECK(evaluated.formatted[4] == "()");
  CHECK(evaluated.formatted[5] == "()");
  CHECK(evaluated.resources.live_evaluation_bytes == 34U);
  CHECK(evaluated.resources.reservation_ordinal == 3U);
  release_rewrite_evaluation_result(evaluated);
  CHECK(evaluated.resources.live_evaluation_bytes == 0U);
}

TEST_CASE("rewrite evaluator applies nested primitives through shared semantics") {
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult evaluated = evaluate_rewrite_source(
      "inc 5\ninc inc 5\nadd[1 2.5]\nadd[10 (1 2 3)]\n"
      "equals[2 (1 2 3 2)]\nnot[(false true)]\niota[3]",
      creation);

  REQUIRE(evaluated.ok);
  if (!evaluated.ok) {
    return;
  }
  REQUIRE(evaluated.formatted.size() == 7U);
  if (evaluated.formatted.size() != 7U) {
    release_rewrite_evaluation_result(evaluated);
    return;
  }
  CHECK(evaluated.formatted[0] == "6");
  CHECK(evaluated.formatted[1] == "7");
  CHECK(evaluated.formatted[2] == "3.5");
  CHECK(evaluated.formatted[3] == "(11 12 13)");
  CHECK(evaluated.formatted[4] == "(false true false true)");
  CHECK(evaluated.formatted[5] == "(true false)");
  CHECK(evaluated.formatted[6] == "(1 2 3)");
  CHECK(evaluated.scalar_kernel_invocations == 13U);
  CHECK(evaluated.resources.work_units == 16U);
  CHECK(evaluated.resources.live_evaluation_bytes == 54U);
  release_rewrite_evaluation_result(evaluated);
  CHECK(evaluated.resources.live_evaluation_bytes == 0U);
}

TEST_CASE("rewrite evaluator locates structured runtime diagnostics from spans") {
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};

  RewriteEvaluationResult arity =
      evaluate_rewrite_source("add[true]", creation);
  REQUIRE_FALSE(arity.ok);
  CHECK(arity.diagnostic.error.kind == ErrorKind::arity_error);
  CHECK(span_is(arity.diagnostic.primary, 1U, 1U, 1U, 4U, 1U, 4U));
  CHECK(span_is(arity.diagnostic.primitive_name, 1U, 1U, 1U, 4U, 1U,
                4U));
  CHECK(span_is(arity.diagnostic.call, 1U, 1U, 1U, 10U, 1U, 10U));
  REQUIRE(arity.diagnostic.arguments.size() == 1U);
  CHECK(span_is(arity.diagnostic.arguments[0], 5U, 1U, 5U, 9U, 1U, 9U));
  CHECK(arity.diagnostic.error.location.offset == 1U);

  RewriteEvaluationResult type = evaluate_rewrite_source(
      "true\r\nadd[\r\n  (1 2)\r\n  (true false true)\r\n]", creation);
  REQUIRE_FALSE(type.ok);
  CHECK(type.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(type.diagnostic.error.argument_position == 2U);
  CHECK(span_is(type.diagnostic.primary, 24U, 4U, 3U, 41U, 4U, 20U));
  CHECK(span_is(type.diagnostic.primitive_name, 7U, 2U, 1U, 10U, 2U,
                4U));
  CHECK(span_is(type.diagnostic.call, 7U, 2U, 1U, 44U, 5U, 2U));
  REQUIRE(type.diagnostic.arguments.size() == 2U);
  CHECK(span_is(type.diagnostic.arguments[0], 15U, 3U, 3U, 20U, 3U,
                8U));
  CHECK(span_is(type.diagnostic.arguments[1], 24U, 4U, 3U, 41U, 4U,
                20U));
  CHECK(type.diagnostic.error.location.offset == 24U);
  CHECK(type.diagnostic.error.location.line == 4U);
  CHECK(type.diagnostic.error.location.column == 3U);
  CHECK(type.scalar_kernel_invocations == 0U);
  CHECK(type.values.empty());
  CHECK(type.formatted.empty());

  RewriteEvaluationResult shape = evaluate_rewrite_source(
      "add[(1 2) (10 20 30)]", creation);
  REQUIRE_FALSE(shape.ok);
  CHECK(shape.diagnostic.error.kind == ErrorKind::shape_mismatch);
  CHECK(shape.diagnostic.error.argument_position == 2U);
  CHECK(span_is(shape.diagnostic.primary, 11U, 1U, 11U, 21U, 1U, 21U));
  CHECK(span_is(shape.diagnostic.primitive_name, 1U, 1U, 1U, 4U, 1U,
                4U));
  CHECK(span_is(shape.diagnostic.call, 1U, 1U, 1U, 22U, 1U, 22U));
  REQUIRE(shape.diagnostic.arguments.size() == 2U);
  CHECK(span_is(shape.diagnostic.arguments[0], 5U, 1U, 5U, 10U, 1U,
                10U));
  CHECK(span_is(shape.diagnostic.arguments[1], 11U, 1U, 11U, 21U, 1U,
                21U));
  CHECK(shape.scalar_kernel_invocations == 0U);
  CHECK(shape.values.empty());
  CHECK(shape.formatted.empty());

  RewriteEvaluationResult prefix =
      evaluate_rewrite_source("inc true", creation);
  REQUIRE_FALSE(prefix.ok);
  CHECK(prefix.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(span_is(prefix.diagnostic.primary, 5U, 1U, 5U, 9U, 1U, 9U));
  CHECK(span_is(prefix.diagnostic.primitive_name, 1U, 1U, 1U, 4U, 1U,
                4U));
  CHECK(span_is(prefix.diagnostic.call, 1U, 1U, 1U, 9U, 1U, 9U));
  REQUIRE(prefix.diagnostic.arguments.size() == 1U);
  CHECK(span_is(prefix.diagnostic.arguments[0], 5U, 1U, 5U, 9U, 1U,
                9U));
  CHECK(prefix.scalar_kernel_invocations == 0U);
  CHECK(prefix.values.empty());
  CHECK(prefix.formatted.empty());

  RewriteEvaluationResult domain = evaluate_rewrite_source(
      "add[(9223372036854775807 0) (1 9223372036854775807)]",
      creation);
  REQUIRE_FALSE(domain.ok);
  CHECK(domain.diagnostic.error.kind == ErrorKind::domain_error);
  CHECK(domain.diagnostic.error.element_index == 0U);
  CHECK(domain.diagnostic.error.domain.has_value());
  CHECK(span_is(domain.diagnostic.primary, 1U, 1U, 1U, 4U, 1U, 4U));
  CHECK(span_is(domain.diagnostic.primitive_name, 1U, 1U, 1U, 4U, 1U,
                4U));
  CHECK(span_is(domain.diagnostic.call, 1U, 1U, 1U, 53U, 1U, 53U));
  REQUIRE(domain.diagnostic.arguments.size() == 2U);
  CHECK(span_is(domain.diagnostic.arguments[0], 5U, 1U, 5U, 28U, 1U,
                28U));
  CHECK(span_is(domain.diagnostic.arguments[1], 29U, 1U, 29U, 52U, 1U,
                52U));
  CHECK(domain.resources.live_evaluation_bytes == 0U);

  const RewriteEvaluationCreationData resource_creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{1U}};
  RewriteEvaluationResult resource =
      evaluate_rewrite_source("inc[(1 2)]", resource_creation);
  REQUIRE_FALSE(resource.ok);
  CHECK(resource.diagnostic.error.kind == ErrorKind::resource_error);
  CHECK(span_is(resource.diagnostic.primary, 1U, 1U, 1U, 4U, 1U, 4U));
  CHECK(span_is(resource.diagnostic.primitive_name, 1U, 1U, 1U, 4U, 1U,
                4U));
  CHECK(span_is(resource.diagnostic.call, 1U, 1U, 1U, 11U, 1U, 11U));
  REQUIRE(resource.diagnostic.arguments.size() == 1U);
  CHECK(span_is(resource.diagnostic.arguments[0], 5U, 1U, 5U, 10U, 1U,
                10U));
  CHECK(resource.scalar_kernel_invocations == 0U);
  CHECK(resource.values.empty());
  CHECK(resource.formatted.empty());
  CHECK(resource.resources.live_evaluation_bytes == 0U);

  RewriteEvaluationResult unknown =
      evaluate_rewrite_source("bogus[1]", creation);
  REQUIRE_FALSE(unknown.ok);
  CHECK(unknown.diagnostic.stage == RewriteEvaluationStage::resolution);
  CHECK(unknown.diagnostic.rewrite.error ==
        RewriteParseError::unknown_primitive);
  CHECK(span_is(unknown.diagnostic.primary, 1U, 1U, 1U, 6U, 1U, 6U));

  RewriteEvaluationResult syntax =
      evaluate_rewrite_source("add[1, 2]", creation);
  REQUIRE_FALSE(syntax.ok);
  CHECK(syntax.diagnostic.stage == RewriteEvaluationStage::parse);
  CHECK(syntax.diagnostic.rewrite.error == RewriteParseError::invalid_byte);
  CHECK(span_is(syntax.diagnostic.primary, 6U, 1U, 6U, 7U, 1U, 7U));
}

TEST_CASE("rewrite evaluator completes static analysis before executing roots") {
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult evaluated =
      evaluate_rewrite_source("iota[2]\nadd[1 true]", creation);

  REQUIRE_FALSE(evaluated.ok);
  CHECK(evaluated.values.empty());
  CHECK(evaluated.formatted.empty());
  CHECK(evaluated.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(evaluated.diagnostic.error.argument_position == 2U);
  CHECK(span_is(evaluated.diagnostic.primary, 15U, 2U, 7U, 19U, 2U,
                11U));
  CHECK(evaluated.resources.work_units == 0U);
  CHECK(evaluated.resources.live_evaluation_bytes == 0U);
  CHECK(evaluated.scalar_kernel_invocations == 0U);
}

TEST_CASE("rewrite evaluator enforces cumulative work and live-byte lifetimes") {
  const RewriteEvaluationCreationData work_creation{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::nullopt, std::nullopt, 3U},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult exact =
      evaluate_rewrite_source("inc 1\ninc 2\ninc 3", work_creation);
  REQUIRE(exact.ok);
  CHECK(exact.resources.work_units == 3U);
  release_rewrite_evaluation_result(exact);

  RewriteEvaluationResult reset =
      evaluate_rewrite_source("inc 1\ninc 2\ninc 3", work_creation);
  REQUIRE(reset.ok);
  CHECK(reset.resources.work_units == 3U);
  release_rewrite_evaluation_result(reset);

  RewriteEvaluationResult one_past = evaluate_rewrite_source(
      "inc 1\ninc 2\ninc 3\ninc 4", work_creation);
  REQUIRE_FALSE(one_past.ok);
  CHECK(one_past.values.empty());
  CHECK(one_past.formatted.empty());
  CHECK(one_past.diagnostic.error.kind == ErrorKind::resource_error);
  REQUIRE(one_past.diagnostic.error.resource.has_value());
  CHECK(one_past.diagnostic.error.resource->limit_kind ==
        ResourceLimitKind::max_work_units);
  CHECK(one_past.diagnostic.error.resource->usage_before == 3U);
  CHECK(one_past.diagnostic.error.resource->refused_charge == 1U);
  CHECK(one_past.resources.work_units == 3U);
  CHECK(one_past.scalar_kernel_invocations == 3U);

  const RewriteEvaluationCreationData live_creation{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::nullopt, 16U, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult released =
      evaluate_rewrite_source("inc[inc[(1)]]", live_creation);
  REQUIRE(released.ok);
  CHECK(released.formatted[0] == "(3)");
  CHECK(released.resources.live_evaluation_bytes == 8U);
  CHECK(released.resources.reservation_ordinal == 3U);
  release_rewrite_evaluation_result(released);
  CHECK(released.resources.live_evaluation_bytes == 0U);

  const RewriteEvaluationCreationData live_one_past_creation{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::nullopt, 15U, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult live_one_past = evaluate_rewrite_source(
      "inc[inc[(1)]]", live_one_past_creation);
  REQUIRE_FALSE(live_one_past.ok);
  REQUIRE(live_one_past.diagnostic.error.resource.has_value());
  CHECK(live_one_past.diagnostic.error.resource->limit_kind ==
        ResourceLimitKind::max_live_evaluation_bytes);
  CHECK(live_one_past.diagnostic.error.resource->usage_before == 8U);
  CHECK(live_one_past.diagnostic.error.resource->refused_charge == 8U);
  CHECK(span_is(live_one_past.diagnostic.primary, 5U, 1U, 5U, 8U, 1U,
                8U));
  CHECK(live_one_past.scalar_kernel_invocations == 0U);
  CHECK(live_one_past.resources.live_evaluation_bytes == 0U);
  CHECK(live_one_past.values.empty());
  CHECK(live_one_past.formatted.empty());

  const RewriteEvaluationCreationData retained_creation{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::nullopt, 23U, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult retained =
      evaluate_rewrite_source("iota[2]\niota[1]", retained_creation);
  REQUIRE_FALSE(retained.ok);
  REQUIRE(retained.diagnostic.error.resource.has_value());
  CHECK(retained.diagnostic.error.resource->usage_before == 16U);
  CHECK(retained.diagnostic.error.resource->refused_charge == 8U);
  CHECK(retained.resources.work_units == 2U);
  CHECK(retained.scalar_kernel_invocations == 0U);
  CHECK(retained.resources.live_evaluation_bytes == 0U);
  CHECK(retained.values.empty());
  CHECK(retained.formatted.empty());

  const RewriteEvaluationCreationData empty_creation{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::nullopt, std::nullopt, 0U},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult empty =
      evaluate_rewrite_source(" \t\n\n", empty_creation);
  REQUIRE(empty.ok);
  CHECK(empty.values.empty());
  CHECK(empty.formatted.empty());
  CHECK(empty.resources.live_evaluation_bytes == 0U);
  CHECK(empty.resources.work_units == 0U);
  release_rewrite_evaluation_result(empty);
}

TEST_CASE("rewrite evaluator refuses resources before latent scalar domain work") {
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::nullopt, std::nullopt, 0U},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult evaluated = evaluate_rewrite_source(
      "inc[(9223372036854775807)]", creation);

  REQUIRE_FALSE(evaluated.ok);
  CHECK(evaluated.diagnostic.stage == RewriteEvaluationStage::application);
  CHECK(evaluated.diagnostic.error.kind == ErrorKind::resource_error);
  REQUIRE(evaluated.diagnostic.error.primitive.has_value());
  CHECK(evaluated.diagnostic.error.primitive->name == "inc");
  REQUIRE(evaluated.diagnostic.error.resource.has_value());
  CHECK(evaluated.diagnostic.error.resource->reason ==
        ResourceErrorReason::profile_limit);
  CHECK(evaluated.diagnostic.error.resource->limit_kind ==
        ResourceLimitKind::max_work_units);
  CHECK(evaluated.diagnostic.error.resource->usage_before == 0U);
  CHECK(evaluated.diagnostic.error.resource->refused_charge == 1U);
  CHECK_FALSE(evaluated.diagnostic.error.domain.has_value());
  CHECK(evaluated.scalar_kernel_invocations == 0U);
  CHECK(evaluated.resources.work_units == 0U);
  CHECK(evaluated.resources.reservation_ordinal == 1U);
  CHECK(evaluated.resources.live_evaluation_bytes == 0U);
  CHECK(evaluated.values.empty());
  CHECK(evaluated.formatted.empty());
}

TEST_CASE("rewrite evaluator uses one deterministic allocation seam") {
  const RewriteParseResult parsed_literal = parse_rewrite("(1)");
  REQUIRE(parsed_literal.ok);
  REQUIRE(parsed_literal.program.nodes.size() == 1U);
  EvaluationResources malformed_literal_resources{
      ExecutionProfile::bounded_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}, 0U, 0U, 0U};
  VectorAllocationResult malformed_literal = vector_literal_value(
      malformed_literal_resources, parsed_literal.program,
      parsed_literal.program.nodes[0]);
  REQUIRE_FALSE(malformed_literal.ok);
  CHECK(malformed_literal.error.kind == ErrorKind::invalid_execution_profile);
  REQUIRE(malformed_literal.error.primitive.has_value());
  CHECK(malformed_literal.error.primitive->name == "vector-literal");
  CHECK(malformed_literal_resources.live_evaluation_bytes == 0U);
  CHECK(malformed_literal_resources.work_units == 0U);
  CHECK(malformed_literal_resources.reservation_ordinal == 0U);

  const RewriteEvaluationCreationData vector_exact_creation{
      ExecutionProfile::bounded_v1,
      ResourceLimits{16U, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult vector_exact =
      evaluate_rewrite_source("(1 2)", vector_exact_creation);
  REQUIRE(vector_exact.ok);
  CHECK(vector_exact.resources.live_evaluation_bytes == 16U);
  release_rewrite_evaluation_result(vector_exact);
  CHECK(vector_exact.resources.live_evaluation_bytes == 0U);

  RewriteEvaluationResult vector_one_past =
      evaluate_rewrite_source("(1 2 3)", vector_exact_creation);
  REQUIRE_FALSE(vector_one_past.ok);
  REQUIRE(vector_one_past.diagnostic.error.primitive.has_value());
  CHECK(vector_one_past.diagnostic.error.primitive->name ==
        "vector-literal");
  REQUIRE(vector_one_past.diagnostic.error.resource.has_value());
  CHECK(vector_one_past.diagnostic.error.resource->limit_kind ==
        ResourceLimitKind::max_vector_bytes);
  CHECK(vector_one_past.diagnostic.error.resource->configured_limit == 16U);
  CHECK(vector_one_past.diagnostic.error.resource->refused_charge == 24U);
  CHECK(vector_one_past.scalar_kernel_invocations == 0U);
  CHECK(vector_one_past.resources.live_evaluation_bytes == 0U);
  CHECK(vector_one_past.values.empty());
  CHECK(vector_one_past.formatted.empty());

  const RewriteEvaluationCreationData literal_failure_creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{0U}};
  RewriteEvaluationResult literal =
      evaluate_rewrite_source("(1 2)", literal_failure_creation);
  REQUIRE_FALSE(literal.ok);
  CHECK(literal.diagnostic.stage == RewriteEvaluationStage::literal);
  REQUIRE(literal.diagnostic.error.resource.has_value());
  CHECK(literal.diagnostic.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  REQUIRE(literal.diagnostic.error.primitive.has_value());
  CHECK(literal.diagnostic.error.primitive->name == "vector-literal");
  CHECK(literal.resources.reservation_ordinal == 1U);
  CHECK(literal.resources.live_evaluation_bytes == 0U);
  CHECK(literal.scalar_kernel_invocations == 0U);
  CHECK(literal.values.empty());
  CHECK(literal.formatted.empty());

  const RewriteEvaluationCreationData result_failure_creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{1U}};
  RewriteEvaluationResult lifted =
      evaluate_rewrite_source("inc[(1 2)]", result_failure_creation);
  REQUIRE_FALSE(lifted.ok);
  CHECK(lifted.diagnostic.stage == RewriteEvaluationStage::application);
  REQUIRE(lifted.diagnostic.error.resource.has_value());
  CHECK(lifted.diagnostic.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  CHECK(lifted.resources.reservation_ordinal == 2U);
  CHECK(lifted.resources.live_evaluation_bytes == 0U);
  CHECK(lifted.resources.work_units == 0U);
  CHECK(lifted.scalar_kernel_invocations == 0U);
  CHECK(lifted.values.empty());
  CHECK(lifted.formatted.empty());

  RewriteEvaluationResult structural =
      evaluate_rewrite_source("iota[2]", literal_failure_creation);
  REQUIRE_FALSE(structural.ok);
  REQUIRE(structural.diagnostic.error.resource.has_value());
  CHECK(structural.diagnostic.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  CHECK(structural.resources.reservation_ordinal == 1U);
  CHECK(structural.resources.live_evaluation_bytes == 0U);
  CHECK(structural.scalar_kernel_invocations == 0U);
  CHECK(structural.values.empty());
  CHECK(structural.formatted.empty());
}

#ifndef DOCTEST_CONFIG_DISABLE
TEST_CASE("rewrite evaluator matches the tracked Section 15 and 16 corpus") {
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  for (const RewriteEvaluatorGoldenFixture &fixture :
       rewrite_evaluator_golden_fixtures) {
    INFO(std::string(fixture.name));
    INFO(std::string(fixture.coverage));
    RewriteEvaluationResult evaluated =
        evaluate_rewrite_source(fixture.source, creation);
    REQUIRE(evaluated.ok);
    if (!evaluated.ok) {
      continue;
    }
    REQUIRE(evaluated.values.size() == 1U);
    REQUIRE(evaluated.formatted.size() == 1U);
    if (evaluated.formatted.size() == 1U) {
      CHECK(evaluated.formatted[0] == fixture.formatted);
    }
    if (fixture.formatted == "()") {
      CHECK(evaluated.scalar_kernel_invocations == 0U);
    }
    release_rewrite_evaluation_result(evaluated);
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
  }

  for (const RewriteEvaluatorErrorFixture &fixture :
       rewrite_evaluator_error_fixtures) {
    INFO(std::string(fixture.name));
    INFO(std::string(fixture.coverage));
    RewriteEvaluationResult evaluated =
        evaluate_rewrite_source(fixture.source, creation);
    REQUIRE_FALSE(evaluated.ok);
    CHECK(evaluated.values.empty());
    CHECK(evaluated.formatted.empty());
    CHECK(evaluated.diagnostic.error.kind == fixture.error);
    CHECK(evaluated.diagnostic.error.argument_position.has_value() ==
          fixture.argument_position.has_value());
    if (fixture.argument_position.has_value()) {
      CHECK(evaluated.diagnostic.error.argument_position ==
            *fixture.argument_position);
    }
    CHECK(evaluated.diagnostic.error.element_index.has_value() ==
          fixture.element_index.has_value());
    if (fixture.element_index.has_value()) {
      CHECK(evaluated.diagnostic.error.element_index ==
            *fixture.element_index);
    }
    if (fixture.error == ErrorKind::arity_error ||
        fixture.error == ErrorKind::type_mismatch ||
        fixture.error == ErrorKind::shape_mismatch) {
      CHECK(evaluated.scalar_kernel_invocations == 0U);
    }
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
  }
}
#endif

TEST_CASE("rewrite evaluation matches direct primitive values and errors") {
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};

  RewriteEvaluationResult parsed_nested =
      evaluate_rewrite_source("add[inc[1] inc[2]]", creation);
  REQUIRE(parsed_nested.ok);
  EvaluationResources nested_resources =
      make_trusted_local_resources(AllocationFailureInjection{std::nullopt});
  PrimitiveApplicationContext nested_context{nested_resources, 0U};
  std::vector<Value> first_arguments;
  first_arguments.push_back(make_int_value(1));
  PrimitiveApplicationResult first = apply_primitive(
      nested_context, *find_primitive(PrimitiveId::inc), first_arguments,
      SourceLocation{5U, 1U, 5U});
  REQUIRE(first.ok);
  release_rewrite_values(nested_resources, first_arguments);
  std::vector<Value> second_arguments;
  second_arguments.push_back(make_int_value(2));
  PrimitiveApplicationResult second = apply_primitive(
      nested_context, *find_primitive(PrimitiveId::inc), second_arguments,
      SourceLocation{12U, 1U, 12U});
  REQUIRE(second.ok);
  release_rewrite_values(nested_resources, second_arguments);
  std::vector<Value> nested_arguments;
  nested_arguments.push_back(std::move(first.value));
  nested_arguments.push_back(std::move(second.value));
  PrimitiveApplicationResult direct_nested = apply_primitive(
      nested_context, *find_primitive(PrimitiveId::add), nested_arguments,
      SourceLocation{1U, 1U, 1U});
  REQUIRE(direct_nested.ok);
  ValueFormattingResult direct_nested_format =
      format_value(direct_nested.value);
  REQUIRE(direct_nested_format.ok);
  REQUIRE(parsed_nested.values.size() == 1U);
  REQUIRE(parsed_nested.formatted.size() == 1U);
  CHECK(value_equal(parsed_nested.values[0], direct_nested.value));
  CHECK(parsed_nested.formatted[0] == direct_nested_format.formatted);
  CHECK(parsed_nested.scalar_kernel_invocations ==
        nested_context.scalar_kernel_invocations);
  CHECK(parsed_nested.resources.work_units == nested_resources.work_units);
  release_rewrite_values(nested_resources, nested_arguments);
  destroy_value(direct_nested.value);
  CHECK(nested_resources.live_evaluation_bytes == 0U);
  release_rewrite_evaluation_result(parsed_nested);
  CHECK(parsed_nested.resources.live_evaluation_bytes == 0U);

  RewriteEvaluationResult parsed_arity =
      evaluate_rewrite_source("add[1]", creation);
  REQUIRE_FALSE(parsed_arity.ok);
  EvaluationResources arity_resources =
      make_trusted_local_resources(AllocationFailureInjection{std::nullopt});
  PrimitiveApplicationContext arity_context{arity_resources, 0U};
  std::vector<Value> arity_arguments;
  arity_arguments.push_back(make_int_value(1));
  PrimitiveApplicationResult direct_arity = apply_primitive(
      arity_context, *find_primitive(PrimitiveId::add), arity_arguments,
      SourceLocation{1U, 1U, 1U});
  REQUIRE_FALSE(direct_arity.ok);
  CHECK(structured_error_equal(parsed_arity.diagnostic.error,
                               direct_arity.error));
  CHECK(parsed_arity.scalar_kernel_invocations ==
        arity_context.scalar_kernel_invocations);
  release_rewrite_values(arity_resources, arity_arguments);
  destroy_value(direct_arity.value);
  release_rewrite_evaluation_result(parsed_arity);

  RewriteEvaluationResult parsed_type =
      evaluate_rewrite_source("add[true 1]", creation);
  REQUIRE_FALSE(parsed_type.ok);
  EvaluationResources type_resources =
      make_trusted_local_resources(AllocationFailureInjection{std::nullopt});
  PrimitiveApplicationContext type_context{type_resources, 0U};
  std::vector<Value> type_arguments;
  type_arguments.push_back(make_bool_value(true));
  type_arguments.push_back(make_int_value(1));
  PrimitiveApplicationResult direct_type = apply_primitive(
      type_context, *find_primitive(PrimitiveId::add), type_arguments,
      SourceLocation{1U, 1U, 1U});
  REQUIRE_FALSE(direct_type.ok);
  CHECK(structured_error_equal(parsed_type.diagnostic.error,
                               direct_type.error));
  CHECK(parsed_type.scalar_kernel_invocations ==
        type_context.scalar_kernel_invocations);
  release_rewrite_values(type_resources, type_arguments);
  destroy_value(direct_type.value);
  release_rewrite_evaluation_result(parsed_type);

  RewriteEvaluationResult parsed_shape = evaluate_rewrite_source(
      "add[(1 2) (10 20 30)]", creation);
  REQUIRE_FALSE(parsed_shape.ok);
  EvaluationResources shape_resources =
      make_trusted_local_resources(AllocationFailureInjection{std::nullopt});
  const std::array<std::int64_t, 2> shape_left{{1, 2}};
  const std::array<std::int64_t, 3> shape_right{{10, 20, 30}};
  VectorAllocationResult direct_left = copy_int_vector(
      shape_resources, shape_left, SourceLocation{5U, 1U, 5U},
      "vector-literal");
  VectorAllocationResult direct_right = copy_int_vector(
      shape_resources, shape_right, SourceLocation{11U, 1U, 11U},
      "vector-literal");
  REQUIRE(direct_left.ok);
  REQUIRE(direct_right.ok);
  std::vector<Value> shape_arguments;
  shape_arguments.push_back(std::move(direct_left.value));
  shape_arguments.push_back(std::move(direct_right.value));
  PrimitiveApplicationContext shape_context{shape_resources, 0U};
  PrimitiveApplicationResult direct_shape = apply_primitive(
      shape_context, *find_primitive(PrimitiveId::add), shape_arguments,
      SourceLocation{1U, 1U, 1U});
  REQUIRE_FALSE(direct_shape.ok);
  CHECK(structured_error_equal(parsed_shape.diagnostic.error,
                               direct_shape.error));
  CHECK(parsed_shape.scalar_kernel_invocations ==
        shape_context.scalar_kernel_invocations);
  release_rewrite_values(shape_resources, shape_arguments);
  destroy_value(direct_shape.value);
  CHECK(shape_resources.live_evaluation_bytes == 0U);
  release_rewrite_evaluation_result(parsed_shape);

  RewriteEvaluationResult parsed_domain = evaluate_rewrite_source(
      "add[(0 9223372036854775807) (0 1)]", creation);
  REQUIRE_FALSE(parsed_domain.ok);
  EvaluationResources domain_resources =
      make_trusted_local_resources(AllocationFailureInjection{std::nullopt});
  const std::array<std::int64_t, 2> domain_left{
      {0, std::numeric_limits<std::int64_t>::max()}};
  const std::array<std::int64_t, 2> domain_right{{0, 1}};
  VectorAllocationResult direct_domain_left = copy_int_vector(
      domain_resources, domain_left, SourceLocation{5U, 1U, 5U},
      "vector-literal");
  VectorAllocationResult direct_domain_right = copy_int_vector(
      domain_resources, domain_right, SourceLocation{29U, 1U, 29U},
      "vector-literal");
  REQUIRE(direct_domain_left.ok);
  REQUIRE(direct_domain_right.ok);
  std::vector<Value> domain_arguments;
  domain_arguments.push_back(std::move(direct_domain_left.value));
  domain_arguments.push_back(std::move(direct_domain_right.value));
  PrimitiveApplicationContext domain_context{domain_resources, 0U};
  PrimitiveApplicationResult direct_domain = apply_primitive(
      domain_context, *find_primitive(PrimitiveId::add), domain_arguments,
      SourceLocation{1U, 1U, 1U});
  REQUIRE_FALSE(direct_domain.ok);
  CHECK(structured_error_equal(parsed_domain.diagnostic.error,
                               direct_domain.error));
  CHECK(parsed_domain.diagnostic.error.element_index == 1U);
  REQUIRE(parsed_domain.diagnostic.error.domain.has_value());
  CHECK(parsed_domain.diagnostic.error.domain->reason ==
        DomainErrorReason::integer_overflow);
  CHECK(parsed_domain.scalar_kernel_invocations ==
        domain_context.scalar_kernel_invocations);
  release_rewrite_values(domain_resources, domain_arguments);
  destroy_value(direct_domain.value);
  CHECK(domain_resources.live_evaluation_bytes == 0U);
  release_rewrite_evaluation_result(parsed_domain);

  const RewriteEvaluationCreationData allocation_creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{1U}};
  RewriteEvaluationResult parsed_allocation =
      evaluate_rewrite_source("inc[(1 2)]", allocation_creation);
  REQUIRE_FALSE(parsed_allocation.ok);
  EvaluationResources allocation_resources =
      make_trusted_local_resources(AllocationFailureInjection{1U});
  const std::array<std::int64_t, 2> allocation_input{{1, 2}};
  VectorAllocationResult direct_allocation_input = copy_int_vector(
      allocation_resources, allocation_input, SourceLocation{5U, 1U, 5U},
      "vector-literal");
  REQUIRE(direct_allocation_input.ok);
  std::vector<Value> allocation_arguments;
  allocation_arguments.push_back(std::move(direct_allocation_input.value));
  PrimitiveApplicationContext allocation_context{allocation_resources, 0U};
  PrimitiveApplicationResult direct_allocation = apply_primitive(
      allocation_context, *find_primitive(PrimitiveId::inc),
      allocation_arguments, SourceLocation{1U, 1U, 1U});
  REQUIRE_FALSE(direct_allocation.ok);
  CHECK(structured_error_equal(parsed_allocation.diagnostic.error,
                               direct_allocation.error));
  CHECK(parsed_allocation.resources.work_units ==
        allocation_resources.work_units);
  CHECK(parsed_allocation.resources.reservation_ordinal ==
        allocation_resources.reservation_ordinal);
  CHECK(parsed_allocation.scalar_kernel_invocations ==
        allocation_context.scalar_kernel_invocations);
  release_rewrite_values(allocation_resources, allocation_arguments);
  destroy_value(direct_allocation.value);
  CHECK(allocation_resources.live_evaluation_bytes == 0U);
  release_rewrite_evaluation_result(parsed_allocation);
  CHECK(parsed_allocation.resources.live_evaluation_bytes == 0U);
}

TEST_CASE("rewrite evaluator executes deep programs without recursive evaluation") {
  constexpr std::size_t depth = 4000U;
  std::string source;
  source.reserve(depth * 4U + 1U);
  for (std::size_t index = 0U; index < depth; ++index) {
    source += "inc ";
  }
  source += '1';
  const RewriteEvaluationCreationData creation{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult evaluated =
      evaluate_rewrite_source(source, creation);

  REQUIRE(evaluated.ok);
  REQUIRE(evaluated.formatted.size() == 1U);
  CHECK(evaluated.formatted[0] == "4001");
  CHECK(evaluated.scalar_kernel_invocations == depth);
  CHECK(evaluated.resources.work_units == depth);
  CHECK(evaluated.resources.live_evaluation_bytes == 0U);
  release_rewrite_evaluation_result(evaluated);
}

TEST_CASE("rewrite evaluator clears formatted roots after a formatting failure") {
  const RewriteParseResult parsed = parse_rewrite("1\n2");
  REQUIRE(parsed.ok);
  std::vector<Value> values;
  values.push_back(make_int_value(1));
  values.push_back(make_int_value(2));
  values[1].scalar.boolean = true;
  std::vector<std::string> formatted{"must-not-escape"};
  RewriteEvaluationDiagnostic diagnostic =
      empty_rewrite_evaluation_diagnostic();

  const bool formatted_ok = format_rewrite_root_values(
      parsed.program, values, formatted, diagnostic);

  CHECK_FALSE(formatted_ok);
  CHECK(formatted.empty());
  CHECK(diagnostic.stage == RewriteEvaluationStage::formatting);
  CHECK(diagnostic.formatting_error == ValueFormatError::invalid_value);
  CHECK(diagnostic.formatting_invariant ==
        ValueInvariant::inactive_scalar_field);
  CHECK(span_is(diagnostic.primary, 3U, 2U, 1U, 4U, 2U, 2U));
  EvaluationResources resources =
      make_trusted_local_resources(AllocationFailureInjection{std::nullopt});
  release_rewrite_values(resources, values);
}

} // namespace

ValueResult evaluate_expression(std::string_view source) {
  return evaluate_expression(source,
                             trusted_local_evaluation_configuration());
}

ValueResult evaluate_expression(
    std::string_view source,
    const EvaluationConfiguration &configuration) {
  const RewriteEvaluationCreationData creation{
      configuration.profile, configuration.limits,
      configuration.allocation_failure};
  RewriteEvaluationResult evaluated =
      evaluate_rewrite_source_impl(source, creation, true);
  if (!evaluated.ok) {
    Error error = public_error_from_diagnostic(source, evaluated.diagnostic);
    release_rewrite_evaluation_result(evaluated);
    return ValueResult{false, make_int_value(0), std::move(error)};
  }
  if (evaluated.values.size() != 1U) {
    release_rewrite_evaluation_result(evaluated);
    return ValueResult{
        false, make_int_value(0),
        make_error(ErrorKind::invalid_primitive_table,
                   SourceLocation{1U, 1U, 1U},
                   "single-expression evaluation produced an invalid root count")};
  }
  Value value = std::move(evaluated.values.front());
  evaluated.values.clear();
  evaluated.formatted.clear();
  return ValueResult{
      true, std::move(value),
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U})};
}

ProgramResult evaluate_source(std::string_view source) {
  return evaluate_source(source, trusted_local_evaluation_configuration());
}

ProgramResult evaluate_source(
    std::string_view source,
    const EvaluationConfiguration &configuration) {
  const RewriteEvaluationCreationData creation{
      configuration.profile, configuration.limits,
      configuration.allocation_failure};
  RewriteEvaluationResult evaluated = evaluate_rewrite_source(source, creation);
  if (!evaluated.ok) {
    Error error = public_error_from_diagnostic(source, evaluated.diagnostic);
    release_rewrite_evaluation_result(evaluated);
    return ProgramResult{false, {}, std::move(error)};
  }
  std::vector<Value> values = std::move(evaluated.values);
  evaluated.values.clear();
  evaluated.formatted.clear();
  return ProgramResult{
      true, std::move(values),
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U})};
}

CEmissionResult emit_c_source(std::string_view source) {
  return emit_rewrite_c_source_impl(source,
                                    trusted_local_c_configuration());
}

CEmissionResult emit_c_source(
    std::string_view source,
    const EvaluationConfiguration &configuration) {
  return emit_rewrite_c_source_impl(
      source, c_backend_configuration(configuration));
}

} // namespace bennu
