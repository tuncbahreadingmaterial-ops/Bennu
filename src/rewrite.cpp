#include "bennu/application.hpp"
#include "bennu/c_emitter.hpp"
#include "bennu/evaluator.hpp"
#include "bennu/primitive.hpp"
#include "bennu/resources.hpp"
#include "bennu/value.hpp"
#include "runner_arguments.hpp"
#include "rewrite_c_runtime.hpp"
#include "typed_application.hpp"

#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <bit>
#ifndef DOCTEST_CONFIG_DISABLE
#include <cstdlib>
#include <fstream>
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
  tuple_literal,
  parameter_reference,
  unresolved_name,
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
  RewriteSpan declaration_name_span{};
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
  std::optional<ParameterErrorReason> parameter_reason{};
};

struct RewriteParameterDeclaration {
  std::string name;
  ScalarType type;
  RewriteSpan span;
  RewriteSpan name_span;
  RewriteSpan type_span;
};

struct RewriteParameterHeader {
  bool present;
  RewriteSpan span;
  RewriteSpan keyword_span;
  RewriteSpan opening_span;
  RewriteSpan closing_span;
  std::vector<RewriteParameterDeclaration> declarations;
};

struct RewriteProgram {
  std::string source;
  RewriteParameterHeader parameter_header;
  std::vector<RewriteNode> nodes;
  std::vector<std::size_t> arguments;
  std::vector<RewriteSpan> argument_spans;
  std::vector<std::size_t> roots;
  std::vector<RewriteCall> calls;
  std::vector<std::uint8_t> boolean_elements;
  std::vector<std::int64_t> integer_elements;
  std::vector<double> double_elements;
  std::vector<RewriteSpan> vector_element_spans;
  std::vector<std::size_t> tuple_elements;
  std::vector<RewriteSpan> tuple_element_spans;
};

struct RewriteParseResult {
  bool ok;
  RewriteProgram program;
  RewriteDiagnostic diagnostic;
};

enum class RewriteContextKind {
  bracket_call,
  tuple_literal,
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

void set_parameter_diagnostic(RewriteParseResult &result,
                              ParameterErrorReason reason,
                              RewriteParseError error, RewriteSpan primary,
                              RewriteSpan context, RewriteSpan related) {
  set_diagnostic(result, error, primary, context, related);
  result.diagnostic.parameter_reason = reason;
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

std::size_t finish_tuple(RewriteProgram &program,
                         const std::vector<RewritePendingArgument> &pending,
                         const RewriteContext &context,
                         RewriteSpan closing_span) {
  const std::size_t first_element = program.tuple_elements.size();
  const std::size_t first_element_span = program.tuple_element_spans.size();
  std::size_t pending_index = context.first_pending_argument;
  while (pending_index != no_index) {
    const std::size_t node_index = pending[pending_index].node;
    program.tuple_elements.push_back(node_index);
    program.tuple_element_spans.push_back(program.nodes[node_index].span);
    pending_index = pending[pending_index].next;
  }
  const RewriteSpan complete{context.opening_span.begin, closing_span.end};
  program.nodes.push_back(RewriteNode{
      RewriteNodeKind::tuple_literal,
      complete,
      ScalarType::boolean,
      false,
      0,
      0.0,
      first_element,
      context.argument_count,
      first_element_span,
      0U});
  return program.nodes.size() - 1U;
}

std::size_t finish_delimited(
    RewriteProgram &program,
    const std::vector<RewritePendingArgument> &pending,
    const RewriteContext &context, RewriteSpan closing_span) {
  if (context.kind == RewriteContextKind::tuple_literal) {
    return finish_tuple(program, pending, context, closing_span);
  }
  return finish_call(
      program, pending, context, closing_span,
      RewriteSpan{context.name_span.begin, closing_span.end});
}

RewriteSpan rewrite_context_span(const RewriteTokens &tokens,
                                 const RewriteContext &context) {
  if (context.kind == RewriteContextKind::tuple_literal) {
    return delimited_context_span(tokens, context.opening_token_index,
                                  RewriteTokenKind::right_bracket);
  }
  return bracket_call_context_span(tokens, context);
}

bool token_starts_expression(RewriteTokenKind kind) {
  return is_scalar_token(kind) || kind == RewriteTokenKind::name ||
         kind == RewriteTokenKind::left_bracket ||
         kind == RewriteTokenKind::left_parenthesis ||
         kind == RewriteTokenKind::bool_type ||
         kind == RewriteTokenKind::int_type ||
         kind == RewriteTokenKind::double_type ||
         kind == RewriteTokenKind::malformed_literal ||
         kind == RewriteTokenKind::invalid;
}

std::string_view rewrite_token_spelling(const RewriteTokens &tokens,
                                        const RewriteToken &token) {
  const std::size_t begin = token.span.begin.offset - 1U;
  const std::size_t size = token.span.end.offset - token.span.begin.offset;
  return std::string_view(tokens.source).substr(begin, size);
}

std::optional<ScalarType> parameter_type(RewriteTokenKind kind) {
  if (kind == RewriteTokenKind::bool_type) {
    return ScalarType::boolean;
  }
  if (kind == RewriteTokenKind::int_type) {
    return ScalarType::integer;
  }
  if (kind == RewriteTokenKind::double_type) {
    return ScalarType::double_precision;
  }
  return std::nullopt;
}

bool parameter_name_spelling(std::string_view spelling) {
  if (spelling.empty() || !is_lowercase(spelling.front())) {
    return false;
  }
  for (const char byte : spelling.substr(1U)) {
    if (!is_lowercase(byte) && !is_digit(byte) && byte != '_') {
      return false;
    }
  }
  return true;
}

bool parse_parameter_header(const RewriteTokens &tokens,
                            std::size_t &token_index,
                            RewriteParseResult &result) {
  std::size_t first = token_index;
  while (first < tokens.tokens.size() &&
         (tokens.tokens[first].kind == RewriteTokenKind::horizontal_space ||
          tokens.tokens[first].kind == RewriteTokenKind::line_terminator)) {
    ++first;
  }
  if (first == tokens.tokens.size() ||
      tokens.tokens[first].kind != RewriteTokenKind::name ||
      rewrite_token_spelling(tokens, tokens.tokens[first]) != "parameters") {
    return true;
  }

  const RewriteToken &keyword = tokens.tokens[first];
  if (first + 1U >= tokens.tokens.size() ||
      tokens.tokens[first + 1U].kind != RewriteTokenKind::left_bracket) {
    const RewriteSpan primary =
        first + 1U < tokens.tokens.size()
            ? tokens.tokens[first + 1U].span
            : insertion_span(tokens.end);
    set_parameter_diagnostic(
        result, ParameterErrorReason::expected_header_open,
        RewriteParseError::expected_expression, primary, keyword.span,
        keyword.span);
    return false;
  }

  RewriteParameterHeader &header = result.program.parameter_header;
  header.present = true;
  header.keyword_span = keyword.span;
  header.opening_span = tokens.tokens[first + 1U].span;
  std::size_t index = first + 2U;
  while (index < tokens.tokens.size() &&
         (tokens.tokens[index].kind == RewriteTokenKind::horizontal_space ||
          tokens.tokens[index].kind == RewriteTokenKind::line_terminator)) {
    ++index;
  }
  while (index < tokens.tokens.size() &&
         tokens.tokens[index].kind != RewriteTokenKind::right_bracket) {
    const RewriteToken &name = tokens.tokens[index];
    const std::string_view name_spelling = rewrite_token_spelling(tokens, name);
    if (!parameter_name_spelling(name_spelling)) {
      const ParameterErrorReason reason = parameter_type(name.kind).has_value()
                                              ? ParameterErrorReason::expected_parameter_name
                                              : ParameterErrorReason::unexpected_header_token;
      set_parameter_diagnostic(
          result, reason,
          RewriteParseError::expected_expression, name.span,
          RewriteSpan{keyword.span.begin, name.span.end}, header.opening_span);
      return false;
    }
    ++index;
    if (index == tokens.tokens.size() ||
        (tokens.tokens[index].kind != RewriteTokenKind::horizontal_space &&
         tokens.tokens[index].kind != RewriteTokenKind::line_terminator)) {
      const RewriteSpan primary = index == tokens.tokens.size()
                                      ? insertion_span(tokens.end)
                                      : tokens.tokens[index].span;
      if (index == tokens.tokens.size() ||
          tokens.tokens[index].kind == RewriteTokenKind::right_bracket) {
        set_parameter_diagnostic(
            result, ParameterErrorReason::expected_parameter_type,
            RewriteParseError::expected_expression,
            index == tokens.tokens.size()
                ? primary
                : insertion_span(tokens.tokens[index].span.begin),
            name.span, name.span);
      } else {
        set_parameter_diagnostic(
            result, ParameterErrorReason::unexpected_header_token,
            RewriteParseError::expected_expression, primary, name.span,
            name.span);
      }
      return false;
    }
    while (index < tokens.tokens.size() &&
           (tokens.tokens[index].kind == RewriteTokenKind::horizontal_space ||
            tokens.tokens[index].kind == RewriteTokenKind::line_terminator)) {
      ++index;
    }
    if (index == tokens.tokens.size()) {
      const RewriteSpan primary = insertion_span(tokens.end);
      set_parameter_diagnostic(
          result, ParameterErrorReason::expected_parameter_type,
          RewriteParseError::expected_expression, primary, name.span,
          name.span);
      return false;
    }
    const RewriteToken &type = tokens.tokens[index];
    const std::optional<ScalarType> scalar_type = parameter_type(type.kind);
    if (!scalar_type.has_value()) {
      const bool closing = type.kind == RewriteTokenKind::right_bracket;
      set_parameter_diagnostic(
          result,
          closing ? ParameterErrorReason::expected_parameter_type
                  : ParameterErrorReason::unexpected_header_token,
          RewriteParseError::expected_expression,
          closing ? insertion_span(type.span.begin) : type.span, name.span,
          name.span);
      return false;
    }
    header.declarations.push_back(RewriteParameterDeclaration{
        std::string(name_spelling), *scalar_type,
        RewriteSpan{name.span.begin, type.span.end}, name.span, type.span});
    ++index;
    if (index < tokens.tokens.size() &&
        tokens.tokens[index].kind != RewriteTokenKind::right_bracket &&
        tokens.tokens[index].kind != RewriteTokenKind::horizontal_space &&
        tokens.tokens[index].kind != RewriteTokenKind::line_terminator) {
      set_parameter_diagnostic(
          result, ParameterErrorReason::unexpected_header_token,
          RewriteParseError::expected_expression, tokens.tokens[index].span,
          RewriteSpan{keyword.span.begin, tokens.tokens[index].span.end},
          type.span);
      return false;
    }
    while (index < tokens.tokens.size() &&
           (tokens.tokens[index].kind == RewriteTokenKind::horizontal_space ||
            tokens.tokens[index].kind == RewriteTokenKind::line_terminator)) {
      ++index;
    }
  }
  if (index == tokens.tokens.size()) {
    const RewriteSpan primary = insertion_span(tokens.end);
    set_parameter_diagnostic(
        result, ParameterErrorReason::missing_header_close,
        RewriteParseError::missing_delimiter, primary,
        RewriteSpan{keyword.span.begin, tokens.end}, header.opening_span);
    return false;
  }
  header.closing_span = tokens.tokens[index].span;
  header.span = RewriteSpan{keyword.span.begin, header.closing_span.end};
  ++index;
  while (index < tokens.tokens.size() &&
         tokens.tokens[index].kind == RewriteTokenKind::horizontal_space) {
    ++index;
  }
  if (index < tokens.tokens.size() &&
      tokens.tokens[index].kind != RewriteTokenKind::line_terminator) {
    set_parameter_diagnostic(
        result, ParameterErrorReason::trailing_header_bytes,
        RewriteParseError::trailing_input, tokens.tokens[index].span,
        header.span, header.span);
    return false;
  }
  if (index < tokens.tokens.size()) {
    ++index;
  }
  token_index = index;
  return true;
}

std::optional<std::size_t> find_parameter(
    const RewriteProgram &program, std::string_view name) {
  for (std::size_t index = 0U;
       index < program.parameter_header.declarations.size(); ++index) {
    if (program.parameter_header.declarations[index].name == name) {
      return index;
    }
  }
  return std::nullopt;
}

RewriteNode parameter_reference_node(
    const RewriteParameterDeclaration &declaration, std::size_t parameter_index,
    RewriteSpan reference_span) {
  return RewriteNode{RewriteNodeKind::parameter_reference,
                     reference_span,
                     declaration.type,
                     false,
                     0,
                     0.0,
                     parameter_index,
                     0U,
                     0U,
                     0U,
                     declaration.name_span};
}

RewriteNode unresolved_name_node(RewriteSpan span) {
  return RewriteNode{RewriteNodeKind::unresolved_name,
                     span,
                     ScalarType::boolean,
                     false,
                     0,
                     0.0,
                     0U,
                     0U,
                     0U,
                     0U};
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

  if (!parse_parameter_header(tokens, token_index, result)) {
    return result;
  }

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
        (contexts.back().kind == RewriteContextKind::bracket_call ||
         contexts.back().kind == RewriteContextKind::tuple_literal)) {
      RewriteContext &context = contexts.back();
      if (context.after_argument) {
        if (token_index < tokens.tokens.size() &&
            tokens.tokens[token_index].kind ==
                RewriteTokenKind::right_bracket) {
          const RewriteSpan closing = tokens.tokens[token_index].span;
          ++token_index;
          const RewriteContext completed_context = context;
          contexts.pop_back();
          completed_node =
              finish_delimited(result.program, pending_arguments,
                               completed_context, closing);
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
              rewrite_context_span(tokens, context),
              context.opening_span);
          return result;
        } else if (tokens.tokens[token_index].kind ==
                   RewriteTokenKind::right_parenthesis) {
          set_diagnostic(
              result, RewriteParseError::mismatched_delimiter,
              tokens.tokens[token_index].span,
              rewrite_context_span(tokens, context),
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
              rewrite_context_span(tokens, context),
              context.opening_span);
          return result;
        }
      }
      if (token_index == tokens.tokens.size()) {
        const RewriteSpan insertion = insertion_span(tokens.end);
        set_diagnostic(result, RewriteParseError::missing_delimiter, insertion,
                       rewrite_context_span(tokens, context),
                       context.opening_span);
        return result;
      }
      if (tokens.tokens[token_index].kind ==
          RewriteTokenKind::right_bracket) {
        const RewriteSpan closing = tokens.tokens[token_index].span;
        ++token_index;
        const RewriteContext completed_context = context;
        contexts.pop_back();
        completed_node =
            finish_delimited(result.program, pending_arguments,
                             completed_context, closing);
        have_expression = true;
        continue;
      }
      if (tokens.tokens[token_index].kind ==
          RewriteTokenKind::right_parenthesis) {
        set_diagnostic(
            result, RewriteParseError::mismatched_delimiter,
            tokens.tokens[token_index].span,
            rewrite_context_span(tokens, context),
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
    if (contexts.empty() && token.kind == RewriteTokenKind::name &&
        rewrite_token_spelling(tokens, token) == "parameters") {
      const bool had_header = result.program.parameter_header.present;
      const RewriteSpan related = had_header
                                      ? result.program.parameter_header.span
                                      : result.program.nodes[
                                            result.program.roots.front()]
                                            .span;
      set_parameter_diagnostic(
          result,
          had_header ? ParameterErrorReason::second_parameter_header
                     : ParameterErrorReason::parameter_header_after_root,
          RewriteParseError::trailing_input, token.span, token.span, related);
      return result;
    }
    if (is_scalar_token(token.kind)) {
      result.program.nodes.push_back(scalar_node(token));
      completed_node = result.program.nodes.size() - 1U;
      ++token_index;
      have_expression = true;
      continue;
    }
    if (token.kind == RewriteTokenKind::left_bracket) {
      contexts.push_back(RewriteContext{
          RewriteContextKind::tuple_literal,
          token.span,
          token.span,
          insertion_span(token.span.end),
          no_index,
          no_index,
          0U,
          token_index,
          false});
      ++token_index;
      while (token_index < tokens.tokens.size() &&
             (tokens.tokens[token_index].kind ==
                  RewriteTokenKind::horizontal_space ||
              tokens.tokens[token_index].kind ==
                  RewriteTokenKind::line_terminator)) {
        ++token_index;
      }
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
      const std::optional<std::size_t> declared_parameter =
          find_parameter(result.program, rewrite_token_spelling(tokens, token));
      const bool inside_bracket =
          !contexts.empty() &&
          (contexts.back().kind == RewriteContextKind::bracket_call ||
           contexts.back().kind == RewriteContextKind::tuple_literal);
      const bool adjacent_bracket =
          token_index + 1U < tokens.tokens.size() &&
          tokens.tokens[token_index + 1U].kind ==
              RewriteTokenKind::left_bracket;
      const bool root_prefix =
          contexts.empty() && token_index + 1U < tokens.tokens.size() &&
          tokens.tokens[token_index + 1U].kind ==
              RewriteTokenKind::horizontal_space;
      if (declared_parameter.has_value() && !adjacent_bracket &&
          (inside_bracket || !root_prefix)) {
        result.program.nodes.push_back(parameter_reference_node(
            result.program.parameter_header.declarations[*declared_parameter],
            *declared_parameter, token.span));
        completed_node = result.program.nodes.size() - 1U;
        ++token_index;
        have_expression = true;
        continue;
      }
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
      result.program.nodes.push_back(unresolved_name_node(token.span));
      completed_node = result.program.nodes.size() - 1U;
      ++token_index;
      have_expression = true;
      continue;
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
  for (std::size_t index = 0U;
       index < program.parameter_header.declarations.size(); ++index) {
    const RewriteParameterDeclaration &declaration =
        program.parameter_header.declarations[index];
    for (std::size_t earlier = 0U; earlier < index; ++earlier) {
      if (program.parameter_header.declarations[earlier].name ==
          declaration.name) {
        RewriteDiagnostic diagnostic{
            RewriteParseError::unknown_primitive, declaration.name_span,
            program.parameter_header.span,
            program.parameter_header.declarations[earlier].name_span};
        diagnostic.parameter_reason =
            ParameterErrorReason::duplicate_parameter_name;
        return RewriteResolutionResult{false, std::move(diagnostic)};
      }
    }
    const bool reserved =
        declaration.name == "parameters" || declaration.name == "fanout" ||
        declaration.name == "true" || declaration.name == "false" ||
        declaration.name == "inf" || declaration.name == "nan" ||
        find_primitive(declaration.name) != nullptr;
    if (reserved) {
      RewriteDiagnostic diagnostic{RewriteParseError::unknown_primitive,
                                   declaration.name_span,
                                   program.parameter_header.span,
                                   declaration.name_span};
      diagnostic.parameter_reason =
          ParameterErrorReason::reserved_parameter_name;
      return RewriteResolutionResult{false, std::move(diagnostic)};
    }
  }

  std::vector<PrimitiveId> resolved_ids;
  resolved_ids.reserve(program.calls.size());
  std::optional<RewriteDiagnostic> first_unknown;
  for (const RewriteCall &call : program.calls) {
    const std::size_t name_begin = call.name_span.begin.offset - 1U;
    const std::size_t name_size =
        call.name_span.end.offset - call.name_span.begin.offset;
    const std::string_view name(program.source.data() + name_begin, name_size);
    const PrimitiveDescriptor *descriptor = find_primitive(name);
    if (descriptor == nullptr) {
      RewriteDiagnostic diagnostic{RewriteParseError::unknown_primitive,
                                   call.name_span, call.span, call.name_span};
      if (!first_unknown.has_value() ||
          diagnostic.primary.begin.offset <
              first_unknown->primary.begin.offset) {
        first_unknown = std::move(diagnostic);
      }
      resolved_ids.push_back(PrimitiveId::inc);
      continue;
    }
    resolved_ids.push_back(descriptor->id);
  }
  for (const RewriteNode &node : program.nodes) {
    if (node.kind != RewriteNodeKind::unresolved_name) {
      continue;
    }
    RewriteDiagnostic diagnostic{RewriteParseError::unknown_primitive,
                                 node.span, node.span, node.span};
    if (!first_unknown.has_value() ||
        diagnostic.primary.begin.offset < first_unknown->primary.begin.offset) {
      first_unknown = std::move(diagnostic);
    }
  }
  if (first_unknown.has_value()) {
    return RewriteResolutionResult{false, std::move(*first_unknown)};
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
  ResourceLifetimeObserver lifetime_observer{};
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
  bool has_operand;
  RewriteSpan operand;
  std::size_t formatting_root_position;
  ValueInvariant formatting_invariant;
  ValueFormatError formatting_error;
};

enum class RewriteCardinality {
  scalar,
  static_vector,
  dynamic_vector,
  tuple,
};

enum class RewriteLoweringOperation {
  source_node,
  prepared_value,
  immutable_borrow,
  immutable_borrow_failure,
};

struct RewriteLoweringNode {
  RewriteNodeKind kind;
  RewriteLoweringOperation operation;
  RewriteCardinality cardinality;
  ScalarType element_type;
  std::size_t element_count;
  std::optional<PrimitiveId> primitive_id;
  PrimitiveImplementation implementation;
  bool runtime_shape_check;
  std::size_t first_argument;
  std::size_t argument_count;
  bool spreads_tuple;
  std::size_t spread_operand;
  std::size_t use_count;
  bool retained_root;
  std::size_t first_element;
  std::size_t parameter_index;
  bool boolean;
  std::int64_t integer;
  double double_precision;
  RewriteSpan primary_span;
  RewriteSpan source_span;
  SourceLocation source_location;
  std::string_view admission_point;
  RewriteSpan declaration_name_span;
  TypeArena structural_type;
};

struct RewriteLoweringProgram {
  std::vector<RewriteLoweringNode> nodes;
  std::vector<std::size_t> arguments;
  std::vector<std::size_t> roots;
  std::vector<std::uint8_t> boolean_elements;
  std::vector<std::int64_t> integer_elements;
  std::vector<double> double_elements;
  std::vector<std::size_t> tuple_elements;
  std::vector<RewriteSpan> tuple_element_spans;
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

struct PreparedRewriteValues {
  std::vector<Value> values;
  std::vector<std::uint8_t> present;
  std::optional<std::size_t> fail_at_borrow_consumer;
  std::size_t borrow_consumer_ordinal;
};

std::optional<RewriteLoweringProgram>
clone_rewrite_lowering_program(const RewriteLoweringProgram &source) {
  RewriteLoweringProgram clone;
  clone.arguments = source.arguments;
  clone.roots = source.roots;
  clone.boolean_elements = source.boolean_elements;
  clone.integer_elements = source.integer_elements;
  clone.double_elements = source.double_elements;
  clone.tuple_elements = source.tuple_elements;
  clone.tuple_element_spans = source.tuple_element_spans;
  clone.nodes.reserve(source.nodes.size());
  for (const RewriteLoweringNode &node : source.nodes) {
    TypeConstructionResult structural_type =
        clone_type(node.structural_type);
    if (!structural_type.ok) {
      return std::nullopt;
    }
    clone.nodes.push_back(RewriteLoweringNode{
        node.kind,
        node.operation,
        node.cardinality,
        node.element_type,
        node.element_count,
        node.primitive_id,
        node.implementation,
        node.runtime_shape_check,
        node.first_argument,
        node.argument_count,
        node.spreads_tuple,
        node.spread_operand,
        node.use_count,
        node.retained_root,
        node.first_element,
        node.parameter_index,
        node.boolean,
        node.integer,
        node.double_precision,
        node.primary_span,
        node.source_span,
        node.source_location,
        node.admission_point,
        node.declaration_name_span,
        std::move(structural_type.type)});
  }
  return clone;
}

EvaluationResources make_rewrite_resources(
    const RewriteEvaluationCreationData &creation) {
  EvaluationResources resources;
  if (creation.profile == ExecutionProfile::trusted_local_v1 &&
      !creation.limits.max_vector_bytes.has_value() &&
      !creation.limits.max_live_evaluation_bytes.has_value() &&
      !creation.limits.max_work_units.has_value() &&
      !creation.limits.max_tuple_table_bytes.has_value()) {
    resources = make_trusted_local_resources(creation.allocation_failure);
  } else {
    resources = make_evaluation_resources(
        creation.profile, creation.limits, creation.allocation_failure,
        0U, 0U, 0U);
  }
  if (creation.lifetime_observer.record != nullptr) {
    static_cast<void>(set_evaluation_resource_lifetime_observer(
        resources, creation.lifetime_observer));
  }
  return resources;
}

EvaluationResources invalid_rewrite_resources(
    const RewriteEvaluationCreationData &creation) {
  return EvaluationResources{
      EvaluationResourceOwner{},
      EvaluationResourceStateHandle{},
      creation.profile,
      creation.limits,
      HostResourceErrorReason::none,
      creation.allocation_failure,
      0U,
      0U,
      0U,
      creation.lifetime_observer,
      0U};
}

SourceLocation rewrite_source_location(RewritePosition position) {
  return SourceLocation{position.offset, position.line, position.column};
}

SourceSpan rewrite_source_span(RewriteSpan span) {
  return SourceSpan{rewrite_source_location(span.begin),
                    rewrite_source_location(span.end)};
}

ArgumentScalarType argument_scalar_type(ScalarType type) {
  switch (type) {
  case ScalarType::boolean:
    return ArgumentScalarType::boolean;
  case ScalarType::integer:
    return ArgumentScalarType::integer;
  case ScalarType::double_precision:
    return ArgumentScalarType::double_precision;
  }
  return ArgumentScalarType::unknown;
}

Error argument_error(const RewriteProgram &program, ArgumentErrorReason reason,
                     std::size_t supplied_count, std::size_t position) {
  const std::size_t required_count =
      program.parameter_header.declarations.size();
  SourceLocation location = program.parameter_header.present
                                ? rewrite_source_location(
                                      program.parameter_header.keyword_span.begin)
                                : SourceLocation{1U, 1U, 1U};
  ArgumentErrorContext context{reason, required_count, supplied_count, position};
  if (position >= 1U && position <= required_count) {
    const RewriteParameterDeclaration &declaration =
        program.parameter_header.declarations[position - 1U];
    location = rewrite_source_location(declaration.name_span.begin);
    context.parameter_name = declaration.name;
    context.expected_type = declaration.type;
    context.declaration_span = rewrite_source_span(declaration.span);
  }
  Error error = make_error(ErrorKind::argument_error, location);
  error.argument = std::move(context);
  error.context_span =
      program.parameter_header.present
          ? rewrite_source_span(program.parameter_header.span)
          : SourceSpan{SourceLocation{1U, 1U, 1U},
                       SourceLocation{1U, 1U, 1U}};
  if (position >= 1U && position <= required_count) {
    const RewriteParameterDeclaration &declaration =
        program.parameter_header.declarations[position - 1U];
    error.primary_span = rewrite_source_span(declaration.name_span);
    error.related_span = rewrite_source_span(declaration.span);
  } else if (program.parameter_header.present) {
    error.primary_span =
        rewrite_source_span(program.parameter_header.keyword_span);
  } else {
    error.primary_span = SourceSpan{SourceLocation{1U, 1U, 1U},
                                    SourceLocation{1U, 1U, 1U}};
  }
  return error;
}

Error validate_parameter_values(const RewriteProgram &program,
                                std::span<const Value> values) {
  const std::size_t required_count =
      program.parameter_header.declarations.size();
  if (values.size() < required_count) {
    return argument_error(program, ArgumentErrorReason::missing, values.size(),
                          values.size() + 1U);
  }
  if (values.size() > required_count) {
    return argument_error(program, ArgumentErrorReason::extra, values.size(),
                          required_count + 1U);
  }

  for (std::size_t index = 0U; index < values.size(); ++index) {
    const Value &value = values[index];
    const std::size_t position = index + 1U;
    if (value.container != ContainerKind::scalar &&
        value.container != ContainerKind::vector) {
      Error error = argument_error(
          program, ArgumentErrorReason::invalid_typed_value, values.size(),
          position);
      error.argument->actual_container = ArgumentContainer::unknown;
      error.argument->invalid_value_invariant =
          ValueInvariant::unknown_container;
      return error;
    }
    if (value.container == ContainerKind::vector) {
      Error error = argument_error(
          program, ArgumentErrorReason::container_mismatch, values.size(),
          position);
      error.argument->actual_container = ArgumentContainer::vector;
      return error;
    }

    const ArgumentScalarType actual_type =
        argument_scalar_type(value.scalar.type);
    if (actual_type == ArgumentScalarType::unknown) {
      Error error = argument_error(
          program, ArgumentErrorReason::invalid_typed_value, values.size(),
          position);
      error.argument->actual_container = ArgumentContainer::scalar;
      error.argument->actual_type = ArgumentScalarType::unknown;
      error.argument->invalid_value_invariant =
          ValueInvariant::unknown_scalar_type;
      return error;
    }

    const ValueValidationResult validation = validate_value(value);
    if (!validation.ok) {
      Error error = argument_error(
          program, ArgumentErrorReason::invalid_typed_value, values.size(),
          position);
      error.argument->actual_container = ArgumentContainer::scalar;
      error.argument->actual_type = actual_type;
      error.argument->invalid_value_invariant =
          validation.invariant == ValueInvariant::noncanonical_nan
              ? ValueInvariant::noncanonical_nan
              : ValueInvariant::inactive_scalar_field;
      return error;
    }
    if (value.scalar.type !=
        program.parameter_header.declarations[index].type) {
      Error error = argument_error(
          program, ArgumentErrorReason::type_mismatch, values.size(), position);
      error.argument->actual_container = ArgumentContainer::scalar;
      error.argument->actual_type = actual_type;
      return error;
    }
  }
  return make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U});
}

struct ScalarTextDecodeResult {
  bool ok;
  ArgumentErrorReason reason;
  Value value;
};

ScalarTextDecodeResult decode_scalar_text(
    ScalarType type, std::string_view spelling,
    const RewriteDoubleParser &double_parser) {
  if (type == ScalarType::boolean) {
    if (spelling == "true") {
      return ScalarTextDecodeResult{
          true, ArgumentErrorReason::invalid_literal, make_bool_value(true)};
    }
    if (spelling == "false") {
      return ScalarTextDecodeResult{
          true, ArgumentErrorReason::invalid_literal, make_bool_value(false)};
    }
    return ScalarTextDecodeResult{
        false, ArgumentErrorReason::invalid_literal, make_bool_value(false)};
  }

  if (type == ScalarType::integer) {
    if (!canonical_integer_grammar(spelling)) {
      return ScalarTextDecodeResult{
          false, ArgumentErrorReason::invalid_literal, make_int_value(0)};
    }
    std::int64_t value = 0;
    const std::from_chars_result converted =
        std::from_chars(spelling.data(), spelling.data() + spelling.size(), value);
    if (converted.ec == std::errc::result_out_of_range) {
      return ScalarTextDecodeResult{
          false, ArgumentErrorReason::out_of_range, make_int_value(0)};
    }
    if (converted.ec != std::errc{} ||
        converted.ptr != spelling.data() + spelling.size()) {
      return ScalarTextDecodeResult{
          false, ArgumentErrorReason::invalid_literal, make_int_value(0)};
    }
    return ScalarTextDecodeResult{
        true, ArgumentErrorReason::invalid_literal, make_int_value(value)};
  }

  if (spelling == "inf") {
    return ScalarTextDecodeResult{
        true, ArgumentErrorReason::invalid_literal,
        make_double_value(std::numeric_limits<double>::infinity())};
  }
  if (spelling == "-inf") {
    return ScalarTextDecodeResult{
        true, ArgumentErrorReason::invalid_literal,
        make_double_value(-std::numeric_limits<double>::infinity())};
  }
  if (spelling == "nan") {
    return ScalarTextDecodeResult{
        true, ArgumentErrorReason::invalid_literal,
        make_double_value(std::numeric_limits<double>::quiet_NaN())};
  }
  if (!finite_double_grammar(spelling)) {
    return ScalarTextDecodeResult{
        false, ArgumentErrorReason::invalid_literal, make_double_value(0.0)};
  }

  double value = 0.0;
  const std::from_chars_result converted =
      parse_finite_double(spelling, double_parser, value);
  if (converted.ec == std::errc::result_out_of_range &&
      decimal_rounds_to_zero(spelling)) {
    value = spelling.front() == '-' ? -0.0 : 0.0;
    return ScalarTextDecodeResult{
        true, ArgumentErrorReason::invalid_literal, make_double_value(value)};
  }
  if (converted.ec != std::errc{} ||
      converted.ptr != spelling.data() + spelling.size() ||
      !std::isfinite(value)) {
    return ScalarTextDecodeResult{
        false, ArgumentErrorReason::out_of_range, make_double_value(0.0)};
  }
  return ScalarTextDecodeResult{
      true, ArgumentErrorReason::invalid_literal, make_double_value(value)};
}

struct TextArgumentsDecodeResult {
  bool ok;
  std::vector<Value> values;
  Error error;
};

TextArgumentsDecodeResult decode_parameter_texts(
    const RewriteProgram &program,
    std::span<const std::string_view> arguments) {
  const std::size_t required_count =
      program.parameter_header.declarations.size();
  if (arguments.size() < required_count) {
    return TextArgumentsDecodeResult{
        false, {},
        argument_error(program, ArgumentErrorReason::missing, arguments.size(),
                       arguments.size() + 1U)};
  }
  if (arguments.size() > required_count) {
    return TextArgumentsDecodeResult{
        false, {},
        argument_error(program, ArgumentErrorReason::extra, arguments.size(),
                       required_count + 1U)};
  }

  std::vector<Value> values;
  values.reserve(arguments.size());
  RewriteDoubleParser double_parser{};
  bool has_double_parser = false;
  for (std::size_t index = 0U; index < arguments.size(); ++index) {
    const ScalarType type = program.parameter_header.declarations[index].type;
    if (type == ScalarType::double_precision && !has_double_parser) {
      double_parser = make_rewrite_double_parser();
      has_double_parser = true;
    }
    ScalarTextDecodeResult decoded =
        decode_scalar_text(type, arguments[index], double_parser);
    if (!decoded.ok) {
      destroy_rewrite_double_parser(double_parser);
      return TextArgumentsDecodeResult{
          false, {},
          argument_error(program, decoded.reason, arguments.size(), index + 1U)};
    }
    values.push_back(std::move(decoded.value));
  }
  destroy_rewrite_double_parser(double_parser);
  return TextArgumentsDecodeResult{
      true, std::move(values),
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U})};
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
      false,
      empty,
      0U,
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

std::optional<RewriteSpan>
first_tuple_span(const RewriteProgram &program) {
  std::optional<RewriteSpan> earliest;
  for (const RewriteNode &node : program.nodes) {
    if (node.kind != RewriteNodeKind::tuple_literal) {
      continue;
    }
    if (!earliest.has_value() ||
        node.span.begin.offset < earliest->begin.offset ||
        (node.span.begin.offset == earliest->begin.offset &&
         node.span.end.offset > earliest->end.offset)) {
      earliest = node.span;
    }
  }
  return earliest;
}

std::optional<RewriteEvaluationDiagnostic>
tuple_profile_diagnostic(const RewriteProgram &program,
                         ExecutionProfile profile) {
  const std::optional<RewriteSpan> span = first_tuple_span(program);
  const bool v1 = profile == ExecutionProfile::trusted_local_v1 ||
                  profile == ExecutionProfile::bounded_v1;
  if (!span.has_value() || !v1) {
    return std::nullopt;
  }
  RewriteEvaluationDiagnostic diagnostic =
      empty_rewrite_evaluation_diagnostic();
  diagnostic.stage = RewriteEvaluationStage::resource_admission;
  diagnostic.primary = *span;
  diagnostic.context = *span;
  diagnostic.related = *span;
  diagnostic.error = make_error(
      ErrorKind::profile_error,
      rewrite_source_location(span->begin));
  diagnostic.error.profile = ProfileErrorContext{
      ProfileErrorReason::unsupported_value_kind,
      execution_profile_name(profile),
      TypeKind::tuple};
  return diagnostic;
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
  if (node.cardinality == RewriteCardinality::scalar) {
    return ContainerKind::scalar;
  }
  if (node.cardinality == RewriteCardinality::tuple) {
    return ContainerKind::tuple;
  }
  return ContainerKind::vector;
}

Error lowering_primitive_error(ErrorKind kind,
                               const PrimitiveDescriptor &descriptor,
                               SourceLocation location) {
  Error error = make_error(kind, location);
  error.primitive = make_primitive_error_context(
      descriptor.name, std::optional<PrimitiveId>{descriptor.id});
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

std::optional<std::size_t>
prefix_tuple_operand(const RewriteProgram &program,
                     const RewriteCall &call);
std::size_t semantic_argument_count(const RewriteProgram &program,
                                    const RewriteCall &call);
std::size_t semantic_argument_node(const RewriteProgram &program,
                                   const RewriteCall &call,
                                   std::size_t position);
RewriteSpan semantic_argument_span(const RewriteProgram &program,
                                   const RewriteCall &call,
                                   std::size_t position);

bool lowering_type_accepts(const PrimitiveDescriptor &descriptor,
                           const PrimitiveSignature &signature,
                           std::size_t argument_index,
                           const RewriteLoweringNode &argument) {
  if (argument.cardinality == RewriteCardinality::tuple) {
    return false;
  }
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
  const std::size_t argument_count =
      semantic_argument_count(program, call);
  context.actual_arguments.reserve(argument_count);
  for (std::size_t index = 0U; index < argument_count; ++index) {
    const RewriteLoweringNode &argument =
        lowering.nodes[semantic_argument_node(program, call, index)];
    TypeConstructionResult actual = clone_type(argument.structural_type);
    if (actual.ok) {
      context.actual_arguments.push_back(std::move(actual.type));
    }
  }

  std::vector<const PrimitiveSignature *> candidates;
  for (std::size_t signature_index = 0U;
       signature_index < descriptor.signature_count; ++signature_index) {
    const PrimitiveSignature &signature =
        descriptor.signatures[signature_index];
    if (signature.parameter_count != argument_count) {
      continue;
    }
    TypeErrorSignatureContext accepted;
    accepted.parameters.reserve(signature.parameter_count);
    for (std::size_t parameter_index = 0U;
         parameter_index < signature.parameter_count; ++parameter_index) {
      accepted.parameters.push_back(
          signature.parameters[parameter_index].container ==
                  ContainerKind::scalar
              ? make_scalar_type(signature.parameters[parameter_index].element)
              : make_vector_type(signature.parameters[parameter_index].element));
    }
    accepted.result = signature.result.container == ContainerKind::scalar
                          ? make_scalar_type(signature.result.element)
                          : make_vector_type(signature.result.element);
    context.accepted_signatures.push_back(std::move(accepted));
    candidates.push_back(&signature);
  }
  for (std::size_t argument_index = 0U;
       argument_index < argument_count && !candidates.empty();
       ++argument_index) {
    std::vector<const PrimitiveSignature *> remaining;
    const RewriteLoweringNode &argument =
        lowering.nodes[
            semantic_argument_node(program, call, argument_index)];
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
      semantic_argument_count(program, call), RewriteSpan{});
  for (std::size_t position = 0U;
       position < diagnostic.arguments.size(); ++position) {
    diagnostic.arguments[position] =
        semantic_argument_span(program, call, position);
  }
  const std::optional<std::size_t> spread_operand =
      prefix_tuple_operand(program, call);
  if (spread_operand.has_value()) {
    diagnostic.has_operand = true;
    diagnostic.operand =
        program.argument_spans[call.first_argument];
    diagnostic.related = diagnostic.operand;
  }
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
  const bool tuple = node.kind == RewriteNodeKind::tuple_literal;
  TypeArena structural_type =
      vector ? make_vector_type(node.element_type)
             : make_scalar_type(node.element_type);
  return RewriteLoweringNode{
      node.kind,
      RewriteLoweringOperation::source_node,
      tuple ? RewriteCardinality::tuple
            : vector ? RewriteCardinality::static_vector
                     : RewriteCardinality::scalar,
      node.element_type,
      (vector || tuple) ? node.element_count : 1U,
      std::nullopt,
      PrimitiveImplementation::none,
      false,
      0U,
      0U,
      false,
      0U,
      0U,
      false,
      node.first_element,
      node.kind == RewriteNodeKind::parameter_reference ? node.first_element
                                                        : 0U,
      node.boolean,
      node.integer,
      node.double_precision,
      node.span,
      node.span,
      rewrite_source_location(node.span.begin),
      vector ? std::string_view{"vector-literal"} : std::string_view{},
      node.declaration_name_span,
      std::move(structural_type)};
}

std::optional<std::size_t>
prefix_tuple_operand(const RewriteProgram &program,
                     const RewriteCall &call) {
  if (call.syntax != RewriteCallSyntax::prefix ||
      call.argument_count != 1U ||
      call.first_argument >= program.arguments.size()) {
    return std::nullopt;
  }
  const std::size_t operand = program.arguments[call.first_argument];
  if (operand >= program.nodes.size() ||
      program.nodes[operand].kind != RewriteNodeKind::tuple_literal) {
    return std::nullopt;
  }
  return operand;
}

std::size_t semantic_argument_count(const RewriteProgram &program,
                                    const RewriteCall &call) {
  const std::optional<std::size_t> operand =
      prefix_tuple_operand(program, call);
  return operand.has_value() ? program.nodes[*operand].element_count
                             : call.argument_count;
}

std::size_t semantic_argument_node(const RewriteProgram &program,
                                   const RewriteCall &call,
                                   std::size_t position) {
  const std::optional<std::size_t> operand =
      prefix_tuple_operand(program, call);
  if (!operand.has_value()) {
    return program.arguments[call.first_argument + position];
  }
  const RewriteNode &tuple = program.nodes[*operand];
  return program.tuple_elements[tuple.first_element + position];
}

RewriteSpan semantic_argument_span(const RewriteProgram &program,
                                   const RewriteCall &call,
                                   std::size_t position) {
  const std::optional<std::size_t> operand =
      prefix_tuple_operand(program, call);
  if (!operand.has_value()) {
    return program.argument_spans[call.first_argument + position];
  }
  const RewriteNode &tuple = program.nodes[*operand];
  return program.tuple_element_spans[
      tuple.first_element_span + position];
}

RewriteLoweringResult lower_rewrite_program(const RewriteProgram &program) {
  for (std::size_t root_index = 0U; root_index < program.roots.size();
       ++root_index) {
    const std::size_t root = program.roots[root_index];
    bool duplicate = false;
    for (std::size_t previous = 0U; previous < root_index; ++previous) {
      if (program.roots[previous] == root) {
        duplicate = true;
        break;
      }
    }
    if (root >= program.nodes.size() || duplicate) {
      RewriteEvaluationDiagnostic diagnostic =
          empty_rewrite_evaluation_diagnostic();
      diagnostic.stage = RewriteEvaluationStage::primitive_table;
      diagnostic.error = make_error(
          ErrorKind::invalid_primitive_table, SourceLocation{1U, 1U, 1U},
          duplicate ? "typed rewrite lowering contains a repeated root"
                    : "typed rewrite lowering contains an invalid root");
      return RewriteLoweringResult{false, {}, std::move(diagnostic)};
    }
  }
  RewriteLoweringProgram lowering;
  lowering.arguments = program.arguments;
  lowering.roots = program.roots;
  lowering.boolean_elements = program.boolean_elements;
  lowering.integer_elements = program.integer_elements;
  lowering.double_elements = program.double_elements;
  lowering.tuple_elements = program.tuple_elements;
  lowering.tuple_element_spans = program.tuple_element_spans;
  lowering.nodes.reserve(program.nodes.size());
  for (const RewriteNode &node : program.nodes) {
    lowering.nodes.push_back(base_lowering_node(node));
  }
  for (const std::size_t argument : lowering.arguments) {
    ++lowering.nodes[argument].use_count;
  }
  for (const std::size_t element : lowering.tuple_elements) {
    ++lowering.nodes[element].use_count;
  }
  for (const std::size_t root : lowering.roots) {
    ++lowering.nodes[root].use_count;
    lowering.nodes[root].retained_root = true;
  }

  // Phase 4 records dependency-independent arity candidates. Dependency-
  // available spread candidates join this same ordered category in phase 5.
  std::optional<std::size_t> arity_failure_node;
  std::optional<RewriteEvaluationDiagnostic> arity_failure;
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
    if (prefix_tuple_operand(program, call).has_value()) {
      continue;
    }
    bool arity_exists = false;
    const std::size_t argument_count =
        semantic_argument_count(program, call);
    for (std::size_t signature_index = 0U;
         signature_index < descriptor->signature_count; ++signature_index) {
      if (descriptor->signatures[signature_index].parameter_count ==
          argument_count) {
        arity_exists = true;
        break;
      }
    }
    if (!arity_exists) {
      if (!arity_failure.has_value()) {
        arity_failure_node = node_index;
        arity_failure = lowering_diagnostic(
            program, call,
            lowering_arity_error(
                *descriptor, argument_count,
                rewrite_source_location(call.name_span.begin)));
      }
    }
  }

  // Phase 5 selects every implementation from dependency-available
  // structural types only.
  std::vector<std::uint8_t> type_available(
      program.nodes.size(), std::uint8_t{0U});
  std::optional<RewriteEvaluationDiagnostic> type_failure;
  for (std::size_t node_index = 0U; node_index < program.nodes.size();
       ++node_index) {
    if (arity_failure_node.has_value() &&
        node_index > *arity_failure_node) {
      break;
    }
    const RewriteNode &node = program.nodes[node_index];
    if (node.kind == RewriteNodeKind::tuple_literal) {
      bool children_available = true;
      for (std::size_t element_index = 0U;
           element_index < node.element_count; ++element_index) {
        const std::size_t child =
            program.tuple_elements[node.first_element + element_index];
        if (type_available[child] == std::uint8_t{0U}) {
          children_available = false;
          break;
        }
      }
      if (!children_available) {
        continue;
      }
      std::vector<TypeArena> element_types;
      element_types.reserve(node.element_count);
      for (std::size_t element_index = 0U;
           element_index < node.element_count; ++element_index) {
        const std::size_t child =
            program.tuple_elements[node.first_element + element_index];
        TypeConstructionResult cloned =
            clone_type(lowering.nodes[child].structural_type);
        if (!cloned.ok) {
          RewriteEvaluationDiagnostic diagnostic =
              empty_rewrite_evaluation_diagnostic();
          diagnostic.stage = RewriteEvaluationStage::resource_admission;
          diagnostic.primary = node.span;
          diagnostic.context = node.span;
          Error error = make_error(
              ErrorKind::resource_error,
              rewrite_source_location(node.span.begin));
          error.resource = ResourceErrorContext{
              cloned.resource_error == HostResourceErrorReason::size_overflow
                  ? ResourceErrorReason::size_overflow
                  : ResourceErrorReason::allocation_unavailable,
              node.element_count,
              std::nullopt,
              "typed-lowering",
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt};
          diagnostic.error = std::move(error);
          return RewriteLoweringResult{false, {}, std::move(diagnostic)};
        }
        element_types.push_back(std::move(cloned.type));
      }
      TypeConstructionResult tuple_type = make_tuple_type(element_types);
      if (!tuple_type.ok) {
        RewriteEvaluationDiagnostic diagnostic =
            empty_rewrite_evaluation_diagnostic();
        diagnostic.stage = RewriteEvaluationStage::resource_admission;
        diagnostic.primary = node.span;
        diagnostic.context = node.span;
        Error error = make_error(
            ErrorKind::resource_error,
            rewrite_source_location(node.span.begin));
        error.resource = ResourceErrorContext{
            tuple_type.resource_error == HostResourceErrorReason::size_overflow
                ? ResourceErrorReason::size_overflow
                : ResourceErrorReason::allocation_unavailable,
            node.element_count,
            std::nullopt,
            "typed-lowering",
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
        diagnostic.error = std::move(error);
        return RewriteLoweringResult{false, {}, std::move(diagnostic)};
      }
      lowering.nodes[node_index].structural_type =
          std::move(tuple_type.type);
      type_available[node_index] = std::uint8_t{1U};
      continue;
    }
    if (node.kind != RewriteNodeKind::primitive_call) {
      type_available[node_index] = std::uint8_t{1U};
      continue;
    }
    const RewriteCall &call = program.calls[node.call_index];
    const PrimitiveDescriptor &descriptor = *find_primitive(*call.primitive);
    const std::size_t argument_count =
        semantic_argument_count(program, call);
    const std::optional<std::size_t> spread_operand =
        prefix_tuple_operand(program, call);
    if (spread_operand.has_value() &&
        type_available[*spread_operand] == std::uint8_t{0U}) {
      continue;
    }
    bool arity_exists = false;
    for (std::size_t signature_index = 0U;
         signature_index < descriptor.signature_count; ++signature_index) {
      if (descriptor.signatures[signature_index].parameter_count ==
          argument_count) {
        arity_exists = true;
        break;
      }
    }
    if (!arity_exists) {
      if (spread_operand.has_value() &&
          (!arity_failure_node.has_value() ||
           node_index < *arity_failure_node)) {
        arity_failure_node = node_index;
        arity_failure = lowering_diagnostic(
            program, call,
            lowering_arity_error(
                descriptor, argument_count,
                rewrite_source_location(call.name_span.begin)));
      }
      continue;
    }
    bool arguments_available = true;
    for (std::size_t argument_index = 0U;
         argument_index < argument_count; ++argument_index) {
      const std::size_t argument =
          semantic_argument_node(program, call, argument_index);
      if (type_available[argument] == std::uint8_t{0U}) {
        arguments_available = false;
        break;
      }
    }
    if (!arguments_available) {
      continue;
    }
    const PrimitiveSignature *signature = nullptr;
    if (descriptor.lifting == LiftingMode::none) {
      for (std::size_t signature_index = 0U;
           signature_index < descriptor.signature_count; ++signature_index) {
        const PrimitiveSignature &candidate =
            descriptor.signatures[signature_index];
        bool accepted = candidate.parameter_count == argument_count;
        for (std::size_t argument_index = 0U;
             accepted && argument_index < argument_count;
             ++argument_index) {
          const RewriteLoweringNode &argument = lowering.nodes[
              semantic_argument_node(program, call, argument_index)];
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
      bool has_structural_argument = false;
      if (argument_count <= actual_types.size()) {
        for (std::size_t argument_index = 0U;
             argument_index < argument_count; ++argument_index) {
          const RewriteLoweringNode &argument =
              lowering.nodes[
                  semantic_argument_node(program, call, argument_index)];
          if (argument.cardinality == RewriteCardinality::tuple) {
            has_structural_argument = true;
          }
          actual_types[argument_index] = argument.element_type;
        }
        if (!has_structural_argument) {
          const SignatureSelectionResult selected = select_primitive_signature(
              descriptor,
              std::span<const ScalarType>(actual_types.data(),
                                          argument_count));
          if (selected.status == SignatureSelectionStatus::success) {
            signature = selected.signature;
          }
        }
      }
    }
    if (signature == nullptr) {
      if (!type_failure.has_value()) {
        type_failure = lowering_diagnostic(
            program, call,
            lowering_type_error(program, call, descriptor, lowering));
      }
      continue;
    }
    RewriteLoweringNode &lowered = lowering.nodes[node_index];
    lowered.primitive_id = descriptor.id;
    lowered.implementation = signature->implementation;
    lowered.element_type = signature->result.element;
    lowered.first_argument = call.first_argument;
    lowered.argument_count = argument_count;
    lowered.spreads_tuple = spread_operand.has_value();
    lowered.spread_operand = spread_operand.value_or(0U);
    lowered.primary_span = call.name_span;
    lowered.source_location = rewrite_source_location(call.name_span.begin);
    lowered.admission_point = descriptor.name;
    lowered.cardinality = descriptor.lifting == LiftingMode::none
                              ? RewriteCardinality::dynamic_vector
                              : RewriteCardinality::scalar;
    lowered.structural_type =
        signature->result.container == ContainerKind::scalar
            ? make_scalar_type(signature->result.element)
            : make_vector_type(signature->result.element);
    type_available[node_index] = std::uint8_t{1U};
  }
  if (arity_failure.has_value()) {
    return RewriteLoweringResult{
        false, {}, std::move(*arity_failure)};
  }
  if (type_failure.has_value()) {
    return RewriteLoweringResult{
        false, {}, std::move(*type_failure)};
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
         argument_index < lowered.argument_count; ++argument_index) {
      const RewriteLoweringNode &argument = lowering.nodes[
          lowered.spreads_tuple
              ? program.tuple_elements[
                    program.nodes[lowered.spread_operand].first_element +
                    argument_index]
              : program.arguments[call.first_argument + argument_index]];
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

bool rewrite_lowering_invariants_hold(
    const RewriteProgram &program,
    const RewriteLoweringProgram &lowering) {
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
  const auto doubles_equal = [](std::span<const double> left,
                                std::span<const double> right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
      if (std::bit_cast<std::uint64_t>(left[index]) !=
          std::bit_cast<std::uint64_t>(right[index])) {
        return false;
      }
    }
    return true;
  };
  if (lowering.nodes.size() != program.nodes.size() ||
      lowering.arguments != program.arguments ||
      lowering.roots != program.roots ||
      lowering.boolean_elements != program.boolean_elements ||
      lowering.integer_elements != program.integer_elements ||
      !doubles_equal(lowering.double_elements, program.double_elements) ||
      lowering.tuple_elements != program.tuple_elements ||
      lowering.tuple_element_spans.size() !=
          program.tuple_element_spans.size() ||
      program.arguments.size() != program.argument_spans.size()) {
    return false;
  }
  for (std::size_t index = 0U;
       index < lowering.tuple_element_spans.size(); ++index) {
    if (!spans_equal(lowering.tuple_element_spans[index],
                     program.tuple_element_spans[index])) {
      return false;
    }
  }

  std::vector<std::uint8_t> seen_calls(program.calls.size(),
                                       std::uint8_t{0U});
  std::vector<std::size_t> expected_uses(lowering.nodes.size(), 0U);
  for (std::size_t node_index = 0U; node_index < lowering.nodes.size();
       ++node_index) {
    const RewriteLoweringNode &node = lowering.nodes[node_index];
    const RewriteNode &source_node = program.nodes[node_index];
    bool borrow = false;
    switch (node.operation) {
    case RewriteLoweringOperation::source_node:
    case RewriteLoweringOperation::prepared_value:
      break;
    case RewriteLoweringOperation::immutable_borrow:
    case RewriteLoweringOperation::immutable_borrow_failure:
      borrow = true;
      break;
    default:
      return false;
    }
    if (node.kind != source_node.kind ||
        !spans_equal(node.source_span, source_node.span) ||
        node.source_location.offset != node.primary_span.begin.offset ||
        node.source_location.line != node.primary_span.begin.line ||
        node.source_location.column != node.primary_span.begin.column ||
        !spans_equal(node.declaration_name_span,
                     source_node.declaration_name_span) ||
        node.first_element != source_node.first_element ||
        node.boolean != source_node.boolean ||
        node.integer != source_node.integer ||
        std::bit_cast<std::uint64_t>(node.double_precision) !=
            std::bit_cast<std::uint64_t>(
                source_node.double_precision)) {
      return false;
    }
    if (borrow && source_node.kind != RewriteNodeKind::primitive_call) {
      return false;
    }
    if (source_node.kind == RewriteNodeKind::unresolved_name) {
      return false;
    }
    if (source_node.kind == RewriteNodeKind::tuple_literal) {
      if (node.cardinality != RewriteCardinality::tuple ||
          node.element_type != source_node.element_type ||
          node.element_count != source_node.element_count ||
          node.first_argument != 0U || node.argument_count != 0U ||
          node.primitive_id.has_value() ||
          node.implementation != PrimitiveImplementation::none ||
          node.runtime_shape_check ||
          !spans_equal(node.primary_span, source_node.span) ||
          node.admission_point != std::string_view{} ||
          source_node.first_element > lowering.tuple_elements.size() ||
          source_node.element_count >
              lowering.tuple_elements.size() - source_node.first_element) {
        return false;
      }
      std::vector<TypeArena> element_types;
      element_types.reserve(source_node.element_count);
      for (std::size_t position = 0U;
           position < source_node.element_count; ++position) {
        const std::size_t element =
            lowering.tuple_elements[source_node.first_element + position];
        if (element >= node_index || element >= lowering.nodes.size()) {
          return false;
        }
        TypeConstructionResult element_type =
            clone_type(lowering.nodes[element].structural_type);
        if (!element_type.ok) {
          return false;
        }
        element_types.push_back(std::move(element_type.type));
        ++expected_uses[element];
      }
      TypeConstructionResult expected_type =
          make_tuple_type(element_types);
      if (!expected_type.ok ||
          !structural_type_equal(node.structural_type,
                                 expected_type.type)) {
        return false;
      }
      continue;
    }
    if (source_node.kind != RewriteNodeKind::primitive_call) {
      const bool vector =
          source_node.kind == RewriteNodeKind::vector_literal;
      const bool parameter =
          source_node.kind == RewriteNodeKind::parameter_reference;
      if (node.element_type != source_node.element_type ||
          node.element_count != (vector ? source_node.element_count : 1U) ||
          node.cardinality !=
              (vector ? RewriteCardinality::static_vector
                      : RewriteCardinality::scalar) ||
          node.first_argument != 0U || node.argument_count != 0U ||
          node.primitive_id.has_value() ||
          node.implementation != PrimitiveImplementation::none ||
          node.runtime_shape_check ||
          !spans_equal(node.primary_span, source_node.span) ||
          node.parameter_index !=
              (parameter ? source_node.first_element : 0U) ||
          node.admission_point !=
              (vector ? std::string_view{"vector-literal"}
                      : std::string_view{})) {
        return false;
      }
      if (vector) {
        const std::size_t payload_size =
            source_node.element_type == ScalarType::boolean
                ? program.boolean_elements.size()
                : source_node.element_type == ScalarType::integer
                      ? program.integer_elements.size()
                      : program.double_elements.size();
        if (source_node.first_element > payload_size ||
            source_node.element_count >
                payload_size - source_node.first_element ||
            source_node.first_element_span >
                program.vector_element_spans.size() ||
            source_node.element_count >
                program.vector_element_spans.size() -
                    source_node.first_element_span) {
          return false;
        }
      }
      if (parameter &&
          (source_node.first_element >=
               program.parameter_header.declarations.size() ||
           program.parameter_header.declarations[source_node.first_element]
                   .type != source_node.element_type ||
           !spans_equal(
               program.parameter_header
                   .declarations[source_node.first_element]
                   .name_span,
               source_node.declaration_name_span))) {
        return false;
      }
      const TypeArena expected_type =
          vector ? make_vector_type(source_node.element_type)
                 : make_scalar_type(source_node.element_type);
      if (!structural_type_equal(node.structural_type, expected_type)) {
        return false;
      }
      continue;
    }
    if (source_node.call_index >= program.calls.size() ||
        seen_calls[source_node.call_index] != std::uint8_t{0U}) {
      return false;
    }
    seen_calls[source_node.call_index] = std::uint8_t{1U};
    const RewriteCall &call = program.calls[source_node.call_index];
    const std::optional<std::size_t> spread_operand =
        prefix_tuple_operand(program, call);
    const std::size_t argument_count =
        semantic_argument_count(program, call);
    if (call.first_argument > program.arguments.size() ||
        call.argument_count >
            program.arguments.size() - call.first_argument ||
        node.first_argument != call.first_argument ||
        node.argument_count != argument_count ||
        node.spreads_tuple != spread_operand.has_value() ||
        node.spread_operand != spread_operand.value_or(0U) ||
        !spans_equal(source_node.span, call.span) ||
        !spans_equal(node.primary_span, call.name_span) ||
        !call.primitive.has_value() ||
        node.primitive_id != call.primitive) {
      return false;
    }
    const PrimitiveDescriptor *descriptor =
        find_primitive(*call.primitive);
    if (descriptor == nullptr ||
        node.admission_point != descriptor->name ||
        node.implementation == PrimitiveImplementation::none) {
      return false;
    }
    bool implementation_matches = false;
    ContainerKind implementation_result_container =
        ContainerKind::scalar;
    for (std::size_t signature_index = 0U;
         signature_index < descriptor->signature_count;
         ++signature_index) {
      const PrimitiveSignature &signature =
          descriptor->signatures[signature_index];
      if (signature.parameter_count == argument_count &&
          signature.implementation == node.implementation &&
          signature.result.element == node.element_type) {
        implementation_matches = true;
        implementation_result_container =
            signature.result.container;
        break;
      }
    }
    if (!implementation_matches) {
      return false;
    }
    for (std::size_t position = 0U; position < node.argument_count;
         ++position) {
      const std::size_t argument =
          semantic_argument_node(program, call, position);
      if (argument >= node_index || argument >= lowering.nodes.size() ||
          !spans_equal(
              semantic_argument_span(program, call, position),
              program.nodes[argument].span)) {
        return false;
      }
    }
    for (std::size_t position = 0U; position < call.argument_count;
         ++position) {
      ++expected_uses[
          lowering.arguments[call.first_argument + position]];
    }
    if (!borrow) {
      bool signature_accepts = false;
      for (std::size_t signature_index = 0U;
           signature_index < descriptor->signature_count;
           ++signature_index) {
        const PrimitiveSignature &signature =
            descriptor->signatures[signature_index];
        bool accepts =
            signature.parameter_count == argument_count &&
            signature.implementation == node.implementation;
        for (std::size_t position = 0U;
             accepts && position < argument_count; ++position) {
          accepts = lowering_type_accepts(
              *descriptor, signature, position,
              lowering.nodes[
                  semantic_argument_node(program, call, position)]);
        }
        if (accepts) {
          signature_accepts = true;
          break;
        }
      }
      if (!signature_accepts) {
        return false;
      }
      RewriteCardinality expected_cardinality =
          RewriteCardinality::scalar;
      std::size_t expected_element_count = 1U;
      bool expected_runtime_shape_check = false;
      if (node.implementation ==
          PrimitiveImplementation::iota_integer) {
        expected_cardinality = RewriteCardinality::dynamic_vector;
        expected_element_count = 0U;
      } else {
        std::optional<std::size_t> known_length;
        bool has_dynamic = false;
        std::size_t vector_count = 0U;
        for (std::size_t position = 0U;
             position < argument_count; ++position) {
          const RewriteLoweringNode &argument =
              lowering.nodes[
                  semantic_argument_node(program, call, position)];
          if (argument.cardinality == RewriteCardinality::scalar) {
            continue;
          }
          if (argument.cardinality == RewriteCardinality::tuple) {
            return false;
          }
          ++vector_count;
          if (argument.cardinality ==
              RewriteCardinality::dynamic_vector) {
            has_dynamic = true;
          } else if (!known_length.has_value()) {
            known_length = argument.element_count;
          } else if (*known_length != argument.element_count) {
            return false;
          }
        }
        expected_runtime_shape_check =
            vector_count > 1U && has_dynamic;
        if (vector_count == 0U) {
          expected_cardinality = RewriteCardinality::scalar;
          expected_element_count = 1U;
        } else if (has_dynamic) {
          expected_cardinality =
              RewriteCardinality::dynamic_vector;
          expected_element_count = known_length.value_or(0U);
        } else {
          expected_cardinality =
              RewriteCardinality::static_vector;
          expected_element_count = *known_length;
        }
      }
      if (node.cardinality != expected_cardinality ||
          node.element_count != expected_element_count ||
          node.runtime_shape_check != expected_runtime_shape_check) {
        return false;
      }
    } else if (*call.primitive != PrimitiveId::inc ||
               node.implementation !=
                   PrimitiveImplementation::inc_integer ||
               argument_count != 1U ||
               node.cardinality != RewriteCardinality::scalar ||
               node.element_type != ScalarType::integer ||
               node.element_count != 1U ||
               node.runtime_shape_check) {
      return false;
    }
    const TypeArena expected_type =
        borrow
            ? make_scalar_type(ScalarType::integer)
            : implementation_result_container == ContainerKind::scalar
            ? make_scalar_type(node.element_type)
            : make_vector_type(node.element_type);
    if (node.cardinality == RewriteCardinality::tuple ||
        !structural_type_equal(node.structural_type, expected_type)) {
      return false;
    }
  }
  for (const std::uint8_t seen : seen_calls) {
    if (seen == std::uint8_t{0U}) {
      return false;
    }
  }
  std::vector<std::uint8_t> roots(lowering.nodes.size(),
                                  std::uint8_t{0U});
  std::vector<std::uint8_t> tuple_owned(lowering.nodes.size(),
                                        std::uint8_t{0U});
  for (const RewriteLoweringNode &node : lowering.nodes) {
    if (node.kind != RewriteNodeKind::tuple_literal) {
      continue;
    }
    for (std::size_t position = 0U; position < node.element_count;
         ++position) {
      const std::size_t element =
          lowering.tuple_elements[node.first_element + position];
      if (tuple_owned[element] != std::uint8_t{0U}) {
        return false;
      }
      tuple_owned[element] = std::uint8_t{1U};
    }
  }
  for (const std::size_t root : lowering.roots) {
    if (root >= lowering.nodes.size() ||
        roots[root] != std::uint8_t{0U}) {
      return false;
    }
    roots[root] = std::uint8_t{1U};
    ++expected_uses[root];
  }
  for (std::size_t index = 0U; index < lowering.nodes.size(); ++index) {
    if (tuple_owned[index] != std::uint8_t{0U} &&
        (expected_uses[index] != 1U ||
         roots[index] != std::uint8_t{0U})) {
      return false;
    }
    if (lowering.nodes[index].use_count != expected_uses[index] ||
        lowering.nodes[index].retained_root !=
            (roots[index] != std::uint8_t{0U})) {
      return false;
    }
  }
  return true;
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
  for (std::size_t index = values.size(); index != 0U; --index) {
    Value &value = values[index - 1U];
    if (value.container == ContainerKind::vector) {
      release_vector_reservation(resources, value);
    } else if (value.container == ContainerKind::tuple) {
      release_value_reservations(resources, value);
    } else {
      destroy_value(value);
    }
  }
  values.clear();
  static_cast<void>(refresh_evaluation_resources(resources));
}

void release_rewrite_node_values(EvaluationResources &resources,
                                 std::vector<Value> &values,
                                 std::vector<std::uint8_t> &live) {
  for (std::size_t end = values.size(); end != 0U; --end) {
    const std::size_t index = end - 1U;
    if (live[index] == std::uint8_t{0U}) {
      continue;
    }
    if (values[index].container == ContainerKind::vector) {
      release_vector_reservation(resources, values[index]);
    } else if (values[index].container == ContainerKind::tuple) {
      release_value_reservations(resources, values[index]);
    } else {
      destroy_value(values[index]);
    }
    live[index] = std::uint8_t{0U};
  }
  static_cast<void>(refresh_evaluation_resources(resources));
}

void release_rewrite_node_value(EvaluationResources &resources,
                                std::vector<Value> &values,
                                std::vector<std::uint8_t> &live,
                                std::size_t node_index) {
  if (live[node_index] == std::uint8_t{0U}) {
    return;
  }
  if (values[node_index].container == ContainerKind::vector) {
    release_vector_reservation(resources, values[node_index]);
  } else if (values[node_index].container == ContainerKind::tuple) {
    release_value_reservations(resources, values[node_index]);
  } else {
    destroy_value(values[node_index]);
  }
  live[node_index] = std::uint8_t{0U};
}

bool complete_rewrite_consumer_attempt(
    EvaluationResources &resources, std::span<const std::size_t> arguments,
    std::span<const RewriteLoweringNode> nodes,
    std::vector<std::size_t> &remaining_uses, std::vector<Value> &values,
    std::vector<std::uint8_t> &live) {
  for (std::size_t position = 0U; position < arguments.size(); ++position) {
    const std::size_t argument_node = arguments[position];
    std::size_t prior_occurrences = 0U;
    for (std::size_t prior = 0U; prior < position; ++prior) {
      if (arguments[prior] == argument_node) {
        ++prior_occurrences;
      }
    }
    if (argument_node >= nodes.size() ||
        remaining_uses[argument_node] <= prior_occurrences ||
        live[argument_node] == std::uint8_t{0U}) {
      return false;
    }
  }
  for (const std::size_t argument_node : arguments) {
    --remaining_uses[argument_node];
  }
  for (std::size_t end = arguments.size(); end != 0U; --end) {
    const std::size_t position = end - 1U;
    const std::size_t argument_node = arguments[position];
    bool later_occurrence = false;
    for (std::size_t later = position + 1U; later < arguments.size();
         ++later) {
      if (arguments[later] == argument_node) {
        later_occurrence = true;
        break;
      }
    }
    if (!later_occurrence && remaining_uses[argument_node] == 0U &&
        !nodes[argument_node].retained_root) {
      release_rewrite_node_value(resources, values, live, argument_node);
    }
  }
  return true;
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
      semantic_argument_count(program, call), RewriteSpan{});
  for (std::size_t position = 0U;
       position < diagnostic.arguments.size(); ++position) {
    diagnostic.arguments[position] =
        semantic_argument_span(program, call, position);
  }
  const std::optional<std::size_t> spread_operand =
      prefix_tuple_operand(program, call);
  if (spread_operand.has_value()) {
    diagnostic.has_operand = true;
    diagnostic.operand =
        program.argument_spans[call.first_argument];
    diagnostic.related = diagnostic.operand;
  }

  diagnostic.primary = call.name_span;
  if ((error.kind == ErrorKind::type_mismatch ||
       error.kind == ErrorKind::shape_mismatch ||
       prefix_tuple_operand(program, call).has_value()) &&
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
    const PrimitiveDescriptor &descriptor,
    std::span<const TypedPrimitiveArgument> arguments) {
  if (!call.runtime_shape_check || call.spreads_tuple) {
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
    if (!value_length(*arguments[position].owner, actual_count).ok) {
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
      diagnostic.formatting_root_position = index + 1U;
      diagnostic.formatting_invariant = result.invariant;
      diagnostic.formatting_error = result.error;
      return false;
    }
    formatted.push_back(std::string(result.formatted));
  }
  return true;
}

void release_rewrite_evaluation_result(RewriteEvaluationResult &result) {
  release_rewrite_values(result.resources, result.values);
  result.formatted.clear();
  release_evaluation_resources(result.resources);
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
      error.primitive.has_value()
          ? std::string(error.primitive->name)
          : std::string("evaluation");
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
    std::string_view source, RewriteEvaluationDiagnostic &diagnostic) {
  if (diagnostic.error.kind != ErrorKind::none) {
    Error error = std::move(diagnostic.error);
    if (error.kind == ErrorKind::unknown_name &&
        !error.primitive.has_value()) {
      const std::string name = source_at_span(source, diagnostic.primary);
      error.primitive =
          make_primitive_error_context(name, std::nullopt);
      error.message = "unknown primitive '" + name + "'";
    }
    if (error.message.empty() && error.static_message.empty()) {
      error.message = semantic_error_message(error);
    }
    if (!error.primary_span.has_value()) {
      error.primary_span = rewrite_source_span(diagnostic.primary);
    }
    if (!error.context_span.has_value()) {
      error.context_span = rewrite_source_span(diagnostic.context);
    }
    if (!error.related_span.has_value()) {
      error.related_span = rewrite_source_span(diagnostic.related);
    }
    if (diagnostic.primitive_name.begin.offset !=
        diagnostic.primitive_name.end.offset) {
      error.primitive_span =
          rewrite_source_span(diagnostic.primitive_name);
    }
    if (diagnostic.call.begin.offset != diagnostic.call.end.offset) {
      error.call_span = rewrite_source_span(diagnostic.call);
    }
    if (diagnostic.has_operand) {
      error.operand_span = rewrite_source_span(diagnostic.operand);
    }
    error.semantic_origins.reserve(diagnostic.arguments.size());
    for (const RewriteSpan origin : diagnostic.arguments) {
      error.semantic_origins.push_back(rewrite_source_span(origin));
    }
    return error;
  }

  if (diagnostic.stage == RewriteEvaluationStage::parse ||
      diagnostic.stage == RewriteEvaluationStage::resolution) {
    if (diagnostic.rewrite.parameter_reason.has_value()) {
      const ParameterErrorReason reason =
          *diagnostic.rewrite.parameter_reason;
      const bool declaration_error =
          reason == ParameterErrorReason::duplicate_parameter_name ||
          reason == ParameterErrorReason::reserved_parameter_name;
      Error error = make_error(
          declaration_error ? ErrorKind::invalid_parameter_declaration
                            : ErrorKind::syntax_error,
          rewrite_source_location(diagnostic.primary.begin),
          declaration_error ? "invalid parameter declaration"
                            : "invalid parameter header");
      error.parameter = ParameterErrorContext{
          reason, rewrite_source_span(diagnostic.primary),
          rewrite_source_span(diagnostic.context),
          rewrite_source_span(diagnostic.related)};
      error.primary_span = error.parameter->primary_span;
      error.context_span = error.parameter->context_span;
      error.related_span = error.parameter->related_span;
      return error;
    }
    const RewriteParseError parse_error = diagnostic.rewrite.error;
    Error error = make_error(
        parse_error_kind(parse_error),
        rewrite_source_location(diagnostic.primary.begin),
        parse_error_message(parse_error));
    if (parse_error == RewriteParseError::unknown_primitive) {
      const std::string name = source_at_span(source, diagnostic.primary);
      error.primitive =
          make_primitive_error_context(name, std::nullopt);
      error.message += " '" + name + "'";
    }
    error.primary_span = rewrite_source_span(diagnostic.primary);
    error.context_span = rewrite_source_span(diagnostic.context);
    error.related_span = rewrite_source_span(diagnostic.related);
    return error;
  }

  if (diagnostic.stage == RewriteEvaluationStage::formatting) {
    Error error = make_error(
        ErrorKind::formatting_error,
        rewrite_source_location(diagnostic.primary.begin),
        "evaluated value cannot be formatted canonically");
    error.formatting = FormattingErrorContext{
        diagnostic.formatting_error, diagnostic.formatting_root_position,
        rewrite_source_span(diagnostic.primary),
        diagnostic.formatting_error == ValueFormatError::invalid_value
            ? std::optional<ValueInvariant>{diagnostic.formatting_invariant}
            : std::nullopt};
    error.primary_span = error.formatting->root_span;
    return error;
  }
  return make_error(ErrorKind::invalid_primitive_table,
                    rewrite_source_location(diagnostic.primary.begin),
                    "rewrite evaluation failed internally");
}

CBackendConfiguration trusted_local_c_configuration() {
  return CBackendConfiguration{
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt},
      AllocationFailureInjection{std::nullopt}};
}

EvaluationConfiguration trusted_local_evaluation_configuration() {
  return EvaluationConfiguration{
      ExecutionProfile::trusted_local_v2,
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

std::optional<Error> validate_rewrite_configuration(
    ExecutionProfile profile, const ResourceLimits &limits,
    std::string_view producer_name) {
  const bool has_configured_limit =
      limits.max_vector_bytes.has_value() ||
      limits.max_live_evaluation_bytes.has_value() ||
      limits.max_work_units.has_value() ||
      limits.max_tuple_table_bytes.has_value();
  std::string_view message;
  switch (profile) {
  case ExecutionProfile::trusted_local_v1:
    if (!has_configured_limit) {
      return std::nullopt;
    }
    message = "trusted-local-v1 requires every resource limit to be omitted";
    break;
  case ExecutionProfile::bounded_v1:
    if (has_configured_limit &&
        !limits.max_tuple_table_bytes.has_value()) {
      return std::nullopt;
    }
    message = limits.max_tuple_table_bytes.has_value()
                  ? "bounded-v1 does not support max_tuple_table_bytes"
                  : "bounded-v1 requires at least one configured resource limit";
    break;
  case ExecutionProfile::trusted_local_v2:
    if (!has_configured_limit) {
      return std::nullopt;
    }
    message = "trusted-local-v2 requires every resource limit to be omitted";
    break;
  case ExecutionProfile::bounded_v2:
    if (has_configured_limit) {
      return std::nullopt;
    }
    message = "bounded-v2 requires at least one configured resource limit";
    break;
  default:
    message = "execution profile tag is unknown";
    break;
  }

  Error error = make_error(
      ErrorKind::invalid_execution_profile, SourceLocation{1U, 1U, 1U});
  error.static_message = message;
  error.primitive =
      make_primitive_error_context(producer_name, std::nullopt);
  return error;
}

RewriteEvaluationResult evaluate_rewrite_source_impl(
    std::string_view source, const RewriteEvaluationCreationData &creation,
    bool require_single_root, std::span<const Value> parameter_values,
    std::span<const std::string_view> text_parameter_values,
    bool decode_text_parameters,
    const RewriteProgram *prepared_program,
    const RewriteLoweringProgram *prepared_lowering,
    PreparedRewriteValues *prepared_values,
    const EvaluationResources *prepared_resources) {
  std::optional<Error> configuration_error =
      validate_rewrite_configuration(
          creation.profile, creation.limits, "rewrite-evaluator");
  if (configuration_error.has_value()) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::resource_admission;
    diagnostic.error = std::move(*configuration_error);
    return rewrite_evaluation_failure(
        invalid_rewrite_resources(creation), std::move(diagnostic), 0U);
  }
  EvaluationResources resources =
      prepared_resources == nullptr ? make_rewrite_resources(creation)
                                    : *prepared_resources;
  const bool prepared =
      prepared_program != nullptr && prepared_lowering != nullptr;
  RewriteParseResult parsed = [&]() {
    if (prepared_program != nullptr && prepared_lowering != nullptr) {
      return RewriteParseResult{
          true, *prepared_program,
          RewriteDiagnostic{RewriteParseError::none, {}, {}, {}, {}}};
    }
    return parse_rewrite(source);
  }();
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

  if (require_single_root && parsed.program.parameter_header.present) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::parse;
    diagnostic.primary = parsed.program.parameter_header.keyword_span;
    diagnostic.context = parsed.program.parameter_header.span;
    diagnostic.related = parsed.program.parameter_header.keyword_span;
    diagnostic.rewrite = RewriteDiagnostic{
        RewriteParseError::trailing_input, diagnostic.primary,
        diagnostic.context, diagnostic.related,
        ParameterErrorReason::program_only_parameter_header};
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
      prepared ? RewriteResolutionResult{
                     true, RewriteDiagnostic{RewriteParseError::none, {}, {},
                                             {}, {}}}
               : resolve_rewrite_primitives(parsed.program);
  if (!resolved.ok) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::resolution;
    diagnostic.rewrite = resolved.diagnostic;
    diagnostic.primary = resolved.diagnostic.primary;
    diagnostic.context = resolved.diagnostic.context;
    diagnostic.related = resolved.diagnostic.related;
    if (!resolved.diagnostic.parameter_reason.has_value()) {
      diagnostic.error = make_error(
          ErrorKind::unknown_name,
          rewrite_source_location(resolved.diagnostic.primary.begin));
    }
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

  std::optional<RewriteEvaluationDiagnostic> profile_diagnostic =
      tuple_profile_diagnostic(parsed.program, creation.profile);
  if (profile_diagnostic.has_value()) {
    return rewrite_evaluation_failure(
        resources, std::move(*profile_diagnostic), 0U);
  }

  RewriteLoweringResult lowered = [&]() {
    if (prepared_program != nullptr && prepared_lowering != nullptr) {
      std::optional<RewriteLoweringProgram> cloned =
          clone_rewrite_lowering_program(*prepared_lowering);
      if (!cloned.has_value()) {
        RewriteEvaluationDiagnostic diagnostic =
            empty_rewrite_evaluation_diagnostic();
        diagnostic.stage = RewriteEvaluationStage::resource_admission;
        diagnostic.error = make_error(
            ErrorKind::resource_error, SourceLocation{1U, 1U, 1U},
            "prepared typed rewrite lowering clone failed");
        return RewriteLoweringResult{false, {}, std::move(diagnostic)};
      }
      return RewriteLoweringResult{
          true, std::move(*cloned), empty_rewrite_evaluation_diagnostic()};
    }
    return lower_rewrite_program(parsed.program);
  }();
  if (!lowered.ok) {
    return rewrite_evaluation_failure(resources, std::move(lowered.diagnostic),
                                      0U);
  }
  if (prepared &&
      !rewrite_lowering_invariants_hold(parsed.program, lowered.program)) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.stage = RewriteEvaluationStage::primitive_table;
    diagnostic.error = make_error(
        ErrorKind::invalid_primitive_table, SourceLocation{1U, 1U, 1U},
        "prepared typed rewrite lowering violates flat-program invariants");
    return rewrite_evaluation_failure(resources, std::move(diagnostic), 0U);
  }

  std::vector<Value> decoded_parameter_values;
  Error parameter_error =
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U});
  if (decode_text_parameters) {
    TextArgumentsDecodeResult decoded =
        decode_parameter_texts(parsed.program, text_parameter_values);
    parameter_error = std::move(decoded.error);
    decoded_parameter_values = std::move(decoded.values);
  } else {
    parameter_error = validate_parameter_values(parsed.program, parameter_values);
  }
  if (parameter_error.kind != ErrorKind::none) {
    RewriteEvaluationDiagnostic diagnostic =
        empty_rewrite_evaluation_diagnostic();
    diagnostic.primary = parsed.program.parameter_header.present
                             ? parsed.program.parameter_header.keyword_span
                             : insertion_span(RewritePosition{1U, 1U, 1U});
    diagnostic.context = parsed.program.parameter_header.present
                             ? parsed.program.parameter_header.span
                             : insertion_span(RewritePosition{1U, 1U, 1U});
    diagnostic.error = std::move(parameter_error);
    return rewrite_evaluation_failure(resources, std::move(diagnostic), 0U);
  }
  const std::span<const Value> bound_parameter_values =
      decode_text_parameters
          ? std::span<const Value>(decoded_parameter_values)
          : parameter_values;

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
    const std::size_t argument_count =
        semantic_argument_count(parsed.program, call);
    if (argument_count > maximum_call_arity) {
      maximum_call_arity = argument_count;
    }
  }
  for (const RewriteNode &node : parsed.program.nodes) {
    if (node.kind == RewriteNodeKind::tuple_literal &&
        node.element_count > maximum_call_arity) {
      maximum_call_arity = node.element_count;
    }
  }
  std::vector<TypedPrimitiveArgument> arguments;
  arguments.reserve(maximum_call_arity);
  std::vector<Value> tuple_arguments;
  tuple_arguments.reserve(maximum_call_arity);

  RewriteLoweringProgram lowering = std::move(lowered.program);
  std::vector<std::size_t> remaining_uses;
  remaining_uses.reserve(lowering.nodes.size());
  for (const RewriteLoweringNode &node : lowering.nodes) {
    remaining_uses.push_back(node.use_count);
  }

  std::vector<Value> node_values;
  node_values.reserve(parsed.program.nodes.size());
  for (std::size_t index = 0U; index < parsed.program.nodes.size(); ++index) {
    node_values.push_back(make_int_value(0));
  }
  std::vector<std::uint8_t> node_live(parsed.program.nodes.size(),
                                      std::uint8_t{0U});
  std::size_t scalar_kernel_invocations = 0U;

  for (std::size_t node_index = 0U;
       node_index < parsed.program.nodes.size(); ++node_index) {
    const RewriteNode &node = parsed.program.nodes[node_index];
    const RewriteLoweringNode &lowered_node = lowering.nodes[node_index];
    if (lowered_node.operation == RewriteLoweringOperation::prepared_value) {
      if (prepared_values == nullptr ||
          prepared_values->values.size() != lowering.nodes.size() ||
          prepared_values->present.size() != lowering.nodes.size() ||
          prepared_values->present[node_index] == std::uint8_t{0U}) {
        RewriteEvaluationDiagnostic diagnostic =
            empty_rewrite_evaluation_diagnostic();
        diagnostic.stage = RewriteEvaluationStage::primitive_table;
        diagnostic.error = make_error(
            ErrorKind::invalid_primitive_table, lowered_node.source_location,
            "prepared lowering is missing a node value");
        release_rewrite_node_values(resources, node_values, node_live);
        return rewrite_evaluation_failure(
            resources, std::move(diagnostic), scalar_kernel_invocations);
      }
      const ValueValidationResult validation =
          validate_value(prepared_values->values[node_index]);
      if (!validation.ok) {
        RewriteEvaluationDiagnostic diagnostic =
            empty_rewrite_evaluation_diagnostic();
        diagnostic.stage = RewriteEvaluationStage::primitive_table;
        diagnostic.error = make_error(
            ErrorKind::invalid_primitive_table, lowered_node.source_location,
            "prepared lowering contains an invalid owned value");
        release_rewrite_node_values(resources, node_values, node_live);
        return rewrite_evaluation_failure(
            resources, std::move(diagnostic), scalar_kernel_invocations);
      }
      node_values[node_index] =
          move_value(prepared_values->values[node_index]);
      prepared_values->present[node_index] = std::uint8_t{0U};
      node_live[node_index] = std::uint8_t{1U};
      if (remaining_uses[node_index] == 0U) {
        release_rewrite_node_value(resources, node_values, node_live,
                                   node_index);
      }
      continue;
    }
    if (node.kind == RewriteNodeKind::scalar_literal) {
      node_values[node_index] = scalar_literal_value(node);
      node_live[node_index] = std::uint8_t{1U};
      if (remaining_uses[node_index] == 0U) {
        release_rewrite_node_value(resources, node_values, node_live,
                                   node_index);
      }
      continue;
    }
    if (node.kind == RewriteNodeKind::parameter_reference) {
      const ScalarValue &parameter =
          bound_parameter_values[node.first_element].scalar;
      if (parameter.type == ScalarType::boolean) {
        node_values[node_index] = make_bool_value(parameter.boolean);
      } else if (parameter.type == ScalarType::integer) {
        node_values[node_index] = make_int_value(parameter.integer);
      } else {
        node_values[node_index] = make_double_value(parameter.double_precision);
      }
      node_live[node_index] = std::uint8_t{1U};
      if (remaining_uses[node_index] == 0U) {
        release_rewrite_node_value(resources, node_values, node_live,
                                   node_index);
      }
      continue;
    }
    if (node.kind == RewriteNodeKind::vector_literal) {
      VectorAllocationResult literal =
          vector_literal_value(resources, parsed.program, node);
      if (literal.ok) {
        node_values[node_index] = std::move(literal.value);
        node_live[node_index] = std::uint8_t{1U};
        if (remaining_uses[node_index] == 0U) {
          release_rewrite_node_value(resources, node_values, node_live,
                                     node_index);
        }
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
    if (node.kind == RewriteNodeKind::tuple_literal) {
      bool invalid_forward_use = false;
      for (std::size_t element_index = 0U;
           element_index < node.element_count; ++element_index) {
        const std::size_t element_node =
            parsed.program.tuple_elements[node.first_element + element_index];
        if (node_live[element_node] == std::uint8_t{0U} ||
            remaining_uses[element_node] != 1U) {
          invalid_forward_use = true;
          break;
        }
        tuple_arguments.push_back(move_value(node_values[element_node]));
        node_live[element_node] = std::uint8_t{0U};
        --remaining_uses[element_node];
      }
      if (invalid_forward_use) {
        release_rewrite_values(resources, tuple_arguments);
        RewriteEvaluationDiagnostic diagnostic =
            empty_rewrite_evaluation_diagnostic();
        diagnostic.stage = RewriteEvaluationStage::primitive_table;
        diagnostic.primary = node.span;
        diagnostic.context = node.span;
        diagnostic.error = make_error(
            ErrorKind::invalid_primitive_table,
            rewrite_source_location(node.span.begin));
        release_rewrite_node_values(resources, node_values, node_live);
        return rewrite_evaluation_failure(
            resources, std::move(diagnostic), scalar_kernel_invocations);
      }
      TupleConstructionResult tuple = make_tuple_value(
          resources, tuple_arguments, rewrite_source_location(node.span.begin),
          "tuple-literal");
      if (!tuple.ok) {
        release_rewrite_values(resources, tuple_arguments);
        RewriteEvaluationDiagnostic diagnostic =
            empty_rewrite_evaluation_diagnostic();
        diagnostic.stage = RewriteEvaluationStage::literal;
        diagnostic.primary = node.span;
        diagnostic.context = node.span;
        diagnostic.error = std::move(tuple.error);
        release_rewrite_node_values(resources, node_values, node_live);
        return rewrite_evaluation_failure(
            resources, std::move(diagnostic), scalar_kernel_invocations);
      }
      tuple_arguments.clear();
      node_values[node_index] = move_value(tuple.value);
      node_live[node_index] = std::uint8_t{1U};
      if (remaining_uses[node_index] == 0U) {
        release_rewrite_node_value(resources, node_values, node_live,
                                   node_index);
      }
      continue;
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
          remaining_uses[argument_node] == 0U) {
        invalid_forward_use = true;
        break;
      }
    }
    if (!invalid_forward_use && lowered_node.spreads_tuple) {
      const Value &operand = node_values[lowered_node.spread_operand];
      const ValueTupleArityResult arity = value_tuple_arity(operand);
      if (!arity.ok || arity.arity != lowered_node.argument_count) {
        invalid_forward_use = true;
      } else {
        for (std::size_t position = 0U;
             position < lowered_node.argument_count; ++position) {
          const std::size_t element_node =
              operand.tuple.child_indexes.storage.get()[
                  operand.tuple.first_child + position];
          arguments.push_back(TypedPrimitiveArgument{
              &operand, element_node});
        }
      }
    } else if (!invalid_forward_use) {
      for (std::size_t argument_index = 0U;
           argument_index < call.argument_count; ++argument_index) {
        const std::size_t argument_node =
            parsed.program.arguments[
                call.first_argument + argument_index];
        arguments.push_back(TypedPrimitiveArgument{
            &node_values[argument_node], std::nullopt});
      }
    }
    if (invalid_forward_use) {
      arguments.clear();
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
      const std::span<const std::size_t> argument_nodes(
          parsed.program.arguments.data() + call.first_argument,
          call.argument_count);
      (void)complete_rewrite_consumer_attempt(
          resources, argument_nodes, lowering.nodes, remaining_uses,
          node_values, node_live);
      arguments.clear();
      release_rewrite_node_values(resources, node_values, node_live);
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }
    PrimitiveApplicationResult applied{
        false, make_int_value(0),
        make_error(ErrorKind::invalid_primitive_table,
                   lowered_node.source_location)};
    if (lowered_node.operation ==
            RewriteLoweringOperation::immutable_borrow ||
        lowered_node.operation ==
            RewriteLoweringOperation::immutable_borrow_failure) {
      bool valid_arguments = true;
      for (const TypedPrimitiveArgument &argument : arguments) {
        if (!validate_value(*argument.owner).ok) {
          valid_arguments = false;
          break;
        }
      }
      const std::size_t ordinal =
          prepared_values == nullptr
              ? 0U
              : prepared_values->borrow_consumer_ordinal++;
      const bool injected =
          lowered_node.operation ==
              RewriteLoweringOperation::immutable_borrow_failure ||
          (prepared_values != nullptr &&
           prepared_values->fail_at_borrow_consumer.has_value() &&
           ordinal == *prepared_values->fail_at_borrow_consumer);
      if (valid_arguments && !injected) {
        applied = PrimitiveApplicationResult{
            true, make_int_value(static_cast<std::int64_t>(arguments.size())),
            make_error(ErrorKind::none, lowered_node.source_location)};
      } else {
        applied = PrimitiveApplicationResult{
            false, make_int_value(0),
            make_error(
                valid_arguments ? ErrorKind::domain_error
                                : ErrorKind::invalid_value,
                lowered_node.source_location,
                injected ? "prepared immutable-borrow consumer failure"
                         : "prepared immutable-borrow consumer received an "
                           "invalid value")};
      }
    } else {
      applied = apply_typed_primitive(
          application_context, *descriptor, lowered_node.implementation,
          arguments, rewrite_source_location(call.name_span.begin));
    }
    if (!applied.ok && lowered_node.spreads_tuple &&
        lowered_node.argument_count == 1U &&
        applied.error.kind == ErrorKind::domain_error &&
        !applied.error.argument_position.has_value()) {
      applied.error.argument_position = 1U;
    }
    scalar_kernel_invocations = application_context.scalar_kernel_invocations;
    const std::span<const std::size_t> argument_nodes(
        parsed.program.arguments.data() + call.first_argument,
        call.argument_count);
    const bool completed = complete_rewrite_consumer_attempt(
        resources, argument_nodes, lowering.nodes, remaining_uses,
        node_values, node_live);
    arguments.clear();
    if (!completed) {
      if (applied.ok) {
        std::vector<Value> incomplete;
        incomplete.push_back(std::move(applied.value));
        release_rewrite_values(resources, incomplete);
      }
      Error error = make_error(ErrorKind::invalid_primitive_table,
                               rewrite_source_location(call.name_span.begin));
      RewriteEvaluationDiagnostic diagnostic =
          application_rewrite_diagnostic(parsed.program, call,
                                         std::move(error));
      release_rewrite_node_values(resources, node_values, node_live);
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }
    if (!applied.ok) {
      RewriteEvaluationDiagnostic diagnostic =
          application_rewrite_diagnostic(parsed.program, call,
                                         std::move(applied.error));
      release_rewrite_node_values(resources, node_values, node_live);
      return rewrite_evaluation_failure(
          resources, std::move(diagnostic), scalar_kernel_invocations);
    }
    node_values[node_index] = std::move(applied.value);
    node_live[node_index] = std::uint8_t{1U};
    if (remaining_uses[node_index] == 0U) {
      release_rewrite_node_value(resources, node_values, node_live,
                                 node_index);
    }
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

#ifndef DOCTEST_CONFIG_DISABLE
RewriteEvaluationResult evaluate_prepared_rewrite_program(
    const RewriteProgram &program, const RewriteLoweringProgram &lowering,
    const RewriteEvaluationCreationData &creation,
    PreparedRewriteValues *prepared_values,
    const EvaluationResources *prepared_resources) {
  return evaluate_rewrite_source_impl(
      program.source, creation, false, {}, {}, false, &program, &lowering,
      prepared_values, prepared_resources);
}
#endif

RewriteEvaluationResult evaluate_rewrite_source(
    std::string_view source, const RewriteEvaluationCreationData &creation) {
  return evaluate_rewrite_source_impl(source, creation, false, {}, {}, false,
                                      nullptr, nullptr, nullptr, nullptr);
}

enum class ParameterMetadataPreflightFailure {
  none,
  host_argument_count,
  declaration_count,
  extra_argument_position,
  parameter_index,
  source_coordinate,
  parameter_name_bytes,
  value_slots,
  name_table,
  type_table,
  span_table,
};

struct ParameterMetadataPreflightInput {
  std::size_t declaration_count;
  bool has_parameter_index;
  std::size_t maximum_parameter_index;
  std::size_t maximum_source_coordinate;
  std::size_t maximum_parameter_name_bytes;
};

struct ParameterMetadataRepresentation {
  std::uintmax_t c_size_maximum;
  std::uintmax_t c_unsigned_literal_maximum;
  std::uintmax_t value_slot_bytes;
  std::uintmax_t name_table_slot_bytes;
  std::uintmax_t type_table_slot_bytes;
  std::uintmax_t source_span_bytes;
};

struct ParameterMetadataPreflightResult {
  bool ok;
  ParameterMetadataPreflightFailure failure;
};

struct GeneratedCSourceLocationLayout {
  std::size_t offset;
  std::size_t line;
  std::size_t column;
};

struct GeneratedCSourceSpanLayout {
  GeneratedCSourceLocationLayout begin;
  GeneratedCSourceLocationLayout end;
};

struct GeneratedCValueLayout {
  int container;
  int type;
  std::size_t count;
  std::uint8_t boolean;
  std::int64_t integer;
  double double_precision;
  void *data;
  void *parent;
  std::size_t parent_index;
  std::size_t cleanup_index;
};

ParameterMetadataRepresentation host_parameter_metadata_representation() {
  return ParameterMetadataRepresentation{
      std::numeric_limits<std::size_t>::max(),
      std::numeric_limits<std::uintmax_t>::max(),
      sizeof(GeneratedCValueLayout),
      sizeof(const char *),
      sizeof(const char *),
      sizeof(GeneratedCSourceSpanLayout)};
}

ParameterMetadataPreflightResult parameter_metadata_preflight(
    ParameterMetadataPreflightInput input,
    ParameterMetadataRepresentation representation) {
  if (std::numeric_limits<std::size_t>::digits >
      std::numeric_limits<std::uintmax_t>::digits) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::declaration_count};
  }
  const std::uintmax_t maximum_count_from_argc =
      static_cast<std::uintmax_t>(std::numeric_limits<int>::max()) - 1U;
  if (maximum_count_from_argc > representation.c_size_maximum) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::host_argument_count};
  }
  const std::uintmax_t declaration_count =
      static_cast<std::uintmax_t>(input.declaration_count);
  const std::uintmax_t representable_maximum =
      std::min(representation.c_size_maximum,
               representation.c_unsigned_literal_maximum);
  if (declaration_count > representable_maximum) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::declaration_count};
  }
  if (declaration_count >= representable_maximum) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::extra_argument_position};
  }
  if (input.has_parameter_index) {
    const std::uintmax_t maximum_parameter_index =
        static_cast<std::uintmax_t>(input.maximum_parameter_index);
    if (maximum_parameter_index >= declaration_count ||
        maximum_parameter_index >= representable_maximum) {
      return ParameterMetadataPreflightResult{
          false, ParameterMetadataPreflightFailure::parameter_index};
    }
  }
  if (static_cast<std::uintmax_t>(input.maximum_source_coordinate) >
      representable_maximum) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::source_coordinate};
  }
  const std::uintmax_t maximum_parameter_name_bytes =
      static_cast<std::uintmax_t>(input.maximum_parameter_name_bytes);
  if (maximum_parameter_name_bytes >= representation.c_size_maximum) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::parameter_name_bytes};
  }
  const auto table_fits =
      [declaration_count, &representation](std::uintmax_t width) {
        return width != 0U &&
               declaration_count <=
                   representation.c_size_maximum / width;
      };
  if (!table_fits(representation.value_slot_bytes)) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::value_slots};
  }
  if (!table_fits(representation.name_table_slot_bytes)) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::name_table};
  }
  if (!table_fits(representation.type_table_slot_bytes)) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::type_table};
  }
  if (!table_fits(representation.source_span_bytes)) {
    return ParameterMetadataPreflightResult{
        false, ParameterMetadataPreflightFailure::span_table};
  }
  return ParameterMetadataPreflightResult{
      true, ParameterMetadataPreflightFailure::none};
}

void include_parameter_metadata_position(std::size_t &maximum,
                                         RewritePosition position) {
  maximum = std::max(maximum, position.offset);
  maximum = std::max(maximum, position.line);
  maximum = std::max(maximum, position.column);
}

ParameterMetadataPreflightInput parameter_metadata_input(
    const RewriteParameterHeader &header) {
  ParameterMetadataPreflightInput input{
      header.declarations.size(),
      !header.declarations.empty(),
      header.declarations.empty() ? 0U : header.declarations.size() - 1U,
      0U,
      0U};
  include_parameter_metadata_position(input.maximum_source_coordinate,
                                      header.span.begin);
  include_parameter_metadata_position(input.maximum_source_coordinate,
                                      header.span.end);
  for (const RewriteParameterDeclaration &declaration :
       header.declarations) {
    input.maximum_parameter_name_bytes =
        std::max(input.maximum_parameter_name_bytes,
                 declaration.name.size());
    include_parameter_metadata_position(input.maximum_source_coordinate,
                                        declaration.span.begin);
    include_parameter_metadata_position(input.maximum_source_coordinate,
                                        declaration.span.end);
  }
  return input;
}

std::optional<RewriteEvaluationDiagnostic>
parameter_metadata_preflight_diagnostic(
    RewriteSpan header_span, ParameterMetadataPreflightInput input,
    ParameterMetadataRepresentation representation) {
  const ParameterMetadataPreflightResult preflight =
      parameter_metadata_preflight(input, representation);
  if (preflight.ok) {
    return std::nullopt;
  }
  RewriteEvaluationDiagnostic diagnostic =
      empty_rewrite_evaluation_diagnostic();
  diagnostic.stage = RewriteEvaluationStage::resource_admission;
  diagnostic.primary = header_span;
  diagnostic.context = header_span;
  diagnostic.related = header_span;
  Error error = make_error(
      ErrorKind::resource_error,
      rewrite_source_location(header_span.begin));
  error.resource = ResourceErrorContext{
      ResourceErrorReason::size_overflow,
      input.declaration_count,
      std::nullopt,
      "c-emitter-parameter-metadata",
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  diagnostic.error = std::move(error);
  return diagnostic;
}

void append_c_unsigned(std::string &source, std::size_t value) {
  std::array<char,
             static_cast<std::size_t>(
                 std::numeric_limits<std::size_t>::digits10) +
                 3U>
      digits{};
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
  case PrimitiveImplementation::logical_and_boolean:
    return "BENNU_IMPL_AND_BOOL";
  case PrimitiveImplementation::logical_or_boolean:
    return "BENNU_IMPL_OR_BOOL";
  case PrimitiveImplementation::not_equals_boolean:
    return "BENNU_IMPL_NOT_EQUALS_BOOL";
  case PrimitiveImplementation::not_equals_integer:
    return "BENNU_IMPL_NOT_EQUALS_INT";
  case PrimitiveImplementation::not_equals_double:
    return "BENNU_IMPL_NOT_EQUALS_DOUBLE";
  case PrimitiveImplementation::odd_integer:
    return "BENNU_IMPL_ODD_INT";
  case PrimitiveImplementation::even_integer:
    return "BENNU_IMPL_EVEN_INT";
  case PrimitiveImplementation::is_positive_integer:
    return "BENNU_IMPL_IS_POSITIVE_INT";
  case PrimitiveImplementation::is_positive_double:
    return "BENNU_IMPL_IS_POSITIVE_DOUBLE";
  case PrimitiveImplementation::is_negative_integer:
    return "BENNU_IMPL_IS_NEGATIVE_INT";
  case PrimitiveImplementation::is_negative_double:
    return "BENNU_IMPL_IS_NEGATIVE_DOUBLE";
  case PrimitiveImplementation::less_than_integer:
    return "BENNU_IMPL_LESS_THAN_INT";
  case PrimitiveImplementation::less_than_double:
    return "BENNU_IMPL_LESS_THAN_DOUBLE";
  case PrimitiveImplementation::greater_than_integer:
    return "BENNU_IMPL_GREATER_THAN_INT";
  case PrimitiveImplementation::greater_than_double:
    return "BENNU_IMPL_GREATER_THAN_DOUBLE";
  case PrimitiveImplementation::none:
    break;
  }
  return "0";
}

std::string_view c_primitive_id_name(PrimitiveId id) {
  switch (id) {
  case PrimitiveId::inc:
    return "BENNU_PRIMITIVE_INC";
  case PrimitiveId::add:
    return "BENNU_PRIMITIVE_ADD";
  case PrimitiveId::equals:
    return "BENNU_PRIMITIVE_EQUALS";
  case PrimitiveId::logical_not:
    return "BENNU_PRIMITIVE_NOT";
  case PrimitiveId::iota:
    return "BENNU_PRIMITIVE_IOTA";
  case PrimitiveId::logical_and:
    return "BENNU_PRIMITIVE_AND";
  case PrimitiveId::logical_or:
    return "BENNU_PRIMITIVE_OR";
  case PrimitiveId::not_equals:
    return "BENNU_PRIMITIVE_NOT_EQUALS";
  case PrimitiveId::odd:
    return "BENNU_PRIMITIVE_ODD";
  case PrimitiveId::even:
    return "BENNU_PRIMITIVE_EVEN";
  case PrimitiveId::is_positive:
    return "BENNU_PRIMITIVE_IS_POSITIVE";
  case PrimitiveId::is_negative:
    return "BENNU_PRIMITIVE_IS_NEGATIVE";
  case PrimitiveId::less_than:
    return "BENNU_PRIMITIVE_LESS_THAN";
  case PrimitiveId::greater_than:
    return "BENNU_PRIMITIVE_GREATER_THAN";
  }
  return "BENNU_PRIMITIVE_NONE";
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
  append_presence(configuration.limits.max_tuple_table_bytes);
  append_value(configuration.limits.max_vector_bytes);
  append_value(configuration.limits.max_live_evaluation_bytes);
  append_value(configuration.limits.max_work_units);
  append_value(configuration.limits.max_tuple_table_bytes);
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
  if (configuration.profile == ExecutionProfile::bounded_v1) {
    source += "BENNU_PROFILE_BOUNDED_V1, ";
  } else if (configuration.profile == ExecutionProfile::trusted_local_v2) {
    source += "BENNU_PROFILE_TRUSTED_LOCAL_V2, ";
  } else if (configuration.profile == ExecutionProfile::bounded_v2) {
    source += "BENNU_PROFILE_BOUNDED_V2, ";
  } else {
    source += "BENNU_PROFILE_TRUSTED_LOCAL_V1, ";
  }
  source +=
      "BENNU_LIMIT_NONE, 0U, 0U, 0U, NULL, {0U, 1U, 1U}, "
      "0, 0U, 0, 0U, 0, 0U, BENNU_IMPL_NONE, "
      "{BENNU_INT, UINT8_C(0), INT64_C(0), 0.0}, "
      "{BENNU_INT, UINT8_C(0), INT64_C(0), 0.0}, "
      "BENNU_PRIMITIVE_NONE, {0U, {BENNU_INT, BENNU_INT}, BENNU_INT}, "
      "0U, {{0U, 1U, 1U}, {0U, 1U, 1U}}, "
      "{{0U, 1U, 1U}, {0U, 1U, 1U}}, "
      "{{0U, 1U, 1U}, {0U, 1U, 1U}}, 0, "
      "{{0U, 1U, 1U}, {0U, 1U, 1U}}, 0U, "
      "{{{0U, 1U, 1U}, {0U, 1U, 1U}}, "
      "{{0U, 1U, 1U}, {0U, 1U, 1U}}}};\n";
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

void append_source_span(std::string &source, RewriteSpan span) {
  source += "bennu_source_span(";
  append_source_location(source, rewrite_source_location(span.begin));
  source += ", ";
  append_source_location(source, rewrite_source_location(span.end));
  source.push_back(')');
}

std::string_view public_scalar_type_name(ScalarType type) {
  if (type == ScalarType::boolean) {
    return "Bool";
  }
  if (type == ScalarType::integer) {
    return "Int";
  }
  return "Double";
}

void append_c_source_location_initializer(std::string &source,
                                          RewritePosition position) {
  source += "{";
  append_c_unsigned(source, position.offset);
  source += ", ";
  append_c_unsigned(source, position.line);
  source += ", ";
  append_c_unsigned(source, position.column);
  source += "}";
}

void append_c_source_span_initializer(std::string &source, RewriteSpan span) {
  source += "{";
  append_c_source_location_initializer(source, span.begin);
  source += ", ";
  append_c_source_location_initializer(source, span.end);
  source += "}";
}

void append_argument_adapter(std::string &source,
                             const RewriteProgram &program) {
  const std::size_t parameter_count =
      program.parameter_header.declarations.size();
  if (parameter_count != 0U) {
    source += "static BennuValue bennu_parameters[" +
              std::to_string(parameter_count) + "] = {{0}};\n";
    source += "static const char *bennu_parameter_names[" +
              std::to_string(parameter_count) + "] = {";
    for (std::size_t index = 0U; index < parameter_count; ++index) {
      if (index != 0U) {
        source += ", ";
      }
      source += "\"";
      source += program.parameter_header.declarations[index].name;
      source += "\"";
    }
    source += "};\n";
    source += "static const char *bennu_parameter_type_names[" +
              std::to_string(parameter_count) + "] = {";
    for (std::size_t index = 0U; index < parameter_count; ++index) {
      if (index != 0U) {
        source += ", ";
      }
      source += "\"";
      source += public_scalar_type_name(
          program.parameter_header.declarations[index].type);
      source += "\"";
    }
    source += "};\n";
    source += "static const BennuSourceSpan bennu_parameter_spans[" +
              std::to_string(parameter_count) + "] = {";
    for (std::size_t index = 0U; index < parameter_count; ++index) {
      if (index != 0U) {
        source += ", ";
      }
      append_c_source_span_initializer(
          source, program.parameter_header.declarations[index].span);
    }
    source += "};\n";
  }
  source += "\nstatic int bennu_bind_arguments(int argc, char **argv) {\n"
            "  size_t bennu_supplied = 0U;\n"
            "  (void)bennu_decode_argument;\n"
            "  (void)argv;\n"
            "  if (argc > 1) {\n"
            "    const uintmax_t bennu_host_count = "
            "(uintmax_t)(argc - 1);\n"
            "    if (bennu_host_count > (uintmax_t)SIZE_MAX) {\n"
            "      (void)fputs(\"InternalError\\n\", stderr);\n"
            "      return 0;\n"
            "    }\n"
            "    bennu_supplied = (size_t)bennu_host_count;\n"
            "  }\n";
  if (parameter_count != 0U) {
    source += "  if (bennu_supplied < ";
    append_c_unsigned(source, parameter_count);
    source += ") {\n";
    source +=
        "    const size_t bennu_position = bennu_supplied + 1U;\n"
        "    (void)bennu_report_argument_error(\n"
        "        \"missing\", ";
    append_c_unsigned(source, parameter_count);
    source +=
        ", bennu_supplied, bennu_position,\n"
        "        bennu_parameter_names[bennu_supplied],\n"
        "        bennu_parameter_type_names[bennu_supplied],\n"
        "        &bennu_parameter_spans[bennu_supplied]);\n"
        "    return 0;\n"
        "  }\n";
  }
  source += "  if (bennu_supplied > ";
  append_c_unsigned(source, parameter_count);
  source +=
      ") {\n"
      "    (void)bennu_report_argument_error(\n"
      "        \"extra\", ";
  append_c_unsigned(source, parameter_count);
  source += ", bennu_supplied, ";
  append_c_unsigned(source, parameter_count + 1U);
  source +=
      ", NULL, NULL, NULL);\n"
      "    return 0;\n"
      "  }\n"
      "  if (setlocale(LC_NUMERIC, \"C\") == NULL) {\n"
      "    (void)fputs(\"InternalError\\n\", stderr);\n"
      "    return 0;\n"
      "  }\n";
  for (std::size_t index = 0U; index < parameter_count; ++index) {
    source += "  {\n"
              "    const BennuArgumentDecode bennu_decoded =\n"
              "        bennu_decode_argument(";
    source += c_type_name(program.parameter_header.declarations[index].type);
    source += ", argv == NULL ? NULL : argv[";
    append_c_unsigned(source, index + 1U);
    source += "], &bennu_parameters[";
    append_c_unsigned(source, index);
    source +=
        "]);\n"
        "    if (bennu_decoded != BENNU_ARGUMENT_DECODE_OK) {\n"
        "      (void)bennu_report_argument_error(\n"
        "          bennu_decoded == BENNU_ARGUMENT_DECODE_OUT_OF_RANGE\n"
        "              ? \"out_of_range\" : \"invalid_literal\",\n"
        "          ";
    append_c_unsigned(source, parameter_count);
    source += ", bennu_supplied, ";
    append_c_unsigned(source, index + 1U);
    source += ", bennu_parameter_names[";
    append_c_unsigned(source, index);
    source += "],\n          bennu_parameter_type_names[";
    append_c_unsigned(source, index);
    source += "], &bennu_parameter_spans[";
    append_c_unsigned(source, index);
    source += "]);\n"
              "      return 0;\n"
              "    }\n"
              "  }\n";
  }
  source += "  return 1;\n"
            "}\n\n";
}

void append_spread_provenance_arguments(
    std::string &source, const RewriteLoweringNode &call,
    const RewriteLoweringProgram &program) {
  const RewriteLoweringNode &operand =
      program.nodes[call.spread_operand];
  source += ", ";
  append_source_span(source, call.primary_span);
  source += ", ";
  append_source_span(source, operand.source_span);
  source += ", ";
  append_c_unsigned(source, call.argument_count);
  source += ", ";
  append_source_span(
      source, call.argument_count >= 1U
                  ? program.tuple_element_spans[operand.first_element]
                  : operand.source_span);
  source += ", ";
  append_source_span(
      source, call.argument_count >= 2U
                  ? program.tuple_element_spans[operand.first_element + 1U]
                  : operand.source_span);
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

void append_parameter_node(std::string &source, std::size_t node_index,
                           const RewriteLoweringNode &node) {
  source += "  bennu_values[" + std::to_string(node_index) +
            "] = bennu_parameters[";
  append_c_unsigned(source, node.parameter_index);
  source += "];\n";
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
  append_source_span(source, node.primary_span);
  source += ", ";
  append_source_span(source, node.source_span);
  source += ")) { goto bennu_failure; }\n";
}

void append_tuple_node(std::string &source, std::size_t node_index,
                       const RewriteLoweringNode &node,
                       const RewriteLoweringProgram &program,
                       std::vector<std::size_t> &remaining_uses) {
  source += "  {\n";
  if (node.element_count != 0U) {
    source += "    BennuValue *bennu_tuple_elements_" +
              std::to_string(node_index) + "[] = {";
    for (std::size_t element_index = 0U;
         element_index < node.element_count; ++element_index) {
      if (element_index != 0U) {
        source += ", ";
      }
      const std::size_t child =
          program.tuple_elements[node.first_element + element_index];
      source += "&bennu_values[" + std::to_string(child) + "]";
    }
    source += "};\n";
  }
  source += "    if (!bennu_tuple(&bennu_resources, &bennu_values[" +
            std::to_string(node_index) + "], ";
  source += node.element_count == 0U
                ? "NULL"
                : "bennu_tuple_elements_" + std::to_string(node_index);
  source += ", ";
  append_c_unsigned(source, node.element_count);
  source += ", \"tuple-literal\", ";
  append_source_span(source, node.primary_span);
  source += ", ";
  append_source_span(source, node.source_span);
  source += ")) { goto bennu_failure; }\n"
            "  }\n";
  for (std::size_t element_index = 0U;
       element_index < node.element_count; ++element_index) {
    const std::size_t child =
        program.tuple_elements[node.first_element + element_index];
    --remaining_uses[child];
  }
}

void append_shape_requirement(
    std::string &source, const RewriteLoweringNode &call,
    const RewriteLoweringProgram &program,
    const RewriteLoweringNode &argument, std::size_t argument_node,
    std::size_t argument_position, std::optional<std::size_t> static_count,
    std::optional<std::size_t> dynamic_anchor,
    std::span<const std::size_t> final_use_releases) {
  source += call.spreads_tuple
                ? "  if (!bennu_require_spread_shape(&bennu_resources, \""
                : "  if (!bennu_require_shape(&bennu_resources, \"";
  source += call.admission_point;
  source += "\", ";
  source += c_primitive_id_name(*call.primitive_id);
  source += ", ";
  append_c_unsigned(source, argument_position);
  source += ", ";
  if (static_count.has_value()) {
    append_c_unsigned(source, *static_count);
  } else if (call.spreads_tuple) {
    std::size_t anchor_position = 0U;
    const RewriteLoweringNode &operand =
        program.nodes[call.spread_operand];
    for (; anchor_position < call.argument_count; ++anchor_position) {
      if (program.tuple_elements[
              operand.first_element + anchor_position] ==
          *dynamic_anchor) {
        break;
      }
    }
    source += "((BennuValue *)bennu_values[" +
              std::to_string(call.spread_operand) +
              "].data)[" + std::to_string(anchor_position) + "].count";
  } else {
    source += "bennu_values[" + std::to_string(*dynamic_anchor) + "].count";
  }
  source += ", ";
  if (call.spreads_tuple) {
    source += "&((BennuValue *)bennu_values[" +
              std::to_string(call.spread_operand) +
              "].data)[" + std::to_string(argument_position - 1U) + "]";
  } else {
    source += "&bennu_values[" + std::to_string(argument_node) + "]";
  }
  source += ", ";
  append_source_span(source, argument.source_span);
  source += ", ";
  append_source_span(source, call.source_span);
  if (call.spreads_tuple) {
    append_spread_provenance_arguments(source, call, program);
  }
  source += ")) {\n";
  for (const std::size_t release : final_use_releases) {
    source += "    bennu_release(&bennu_resources, &bennu_values[" +
              std::to_string(release) + "]);\n";
  }
  source += "    goto bennu_failure;\n"
            "  }\n";
}

std::size_t lowered_semantic_argument_node(
    const RewriteLoweringNode &call,
    const RewriteLoweringProgram &program,
    std::size_t position) {
  if (!call.spreads_tuple) {
    return program.arguments[call.first_argument + position];
  }
  const RewriteLoweringNode &operand =
      program.nodes[call.spread_operand];
  return program.tuple_elements[operand.first_element + position];
}

void append_call_shape_checks(std::string &source,
                              const RewriteLoweringNode &call,
                              const RewriteLoweringProgram &program,
                              std::span<const std::size_t>
                                  final_use_releases) {
  if (!call.runtime_shape_check) {
    return;
  }

  bool has_static_anchor = false;
  for (std::size_t position = 0U; position < call.argument_count; ++position) {
    const std::size_t argument_node =
        lowered_semantic_argument_node(call, program, position);
    if (program.nodes[argument_node].cardinality ==
        RewriteCardinality::static_vector) {
      has_static_anchor = true;
      break;
    }
  }

  std::optional<std::size_t> dynamic_anchor;
  for (std::size_t position = 0U; position < call.argument_count; ++position) {
    const std::size_t argument_node =
        lowered_semantic_argument_node(call, program, position);
    const RewriteLoweringNode &argument = program.nodes[argument_node];
    if (argument.cardinality != RewriteCardinality::dynamic_vector) {
      continue;
    }
    if (!has_static_anchor && !dynamic_anchor.has_value()) {
      dynamic_anchor = argument_node;
      continue;
    }
    append_shape_requirement(
        source, call, program, argument, argument_node, position + 1U,
        has_static_anchor ? std::optional<std::size_t>{call.element_count}
                          : std::nullopt,
        dynamic_anchor, final_use_releases);
  }
}

void complete_lowered_consumer_emission(
    const RewriteLoweringNode &node,
    const RewriteLoweringProgram &program,
    std::vector<std::size_t> &remaining_uses,
    std::vector<std::size_t> &final_use_releases) {
  final_use_releases.clear();
  const std::size_t consumed_count =
      node.spreads_tuple ? 1U : node.argument_count;
  for (std::size_t position = 0U; position < consumed_count; ++position) {
    const std::size_t argument_node =
        node.spreads_tuple
            ? node.spread_operand
            : program.arguments[node.first_argument + position];
    --remaining_uses[argument_node];
  }
  for (std::size_t end = consumed_count; end != 0U; --end) {
    const std::size_t position = end - 1U;
    const std::size_t argument_node =
        node.spreads_tuple
            ? node.spread_operand
            : program.arguments[node.first_argument + position];
    bool later_occurrence = false;
    for (std::size_t later = position + 1U;
         later < consumed_count; ++later) {
      const std::size_t later_node =
          node.spreads_tuple
              ? node.spread_operand
              : program.arguments[node.first_argument + later];
      if (later_node == argument_node) {
        later_occurrence = true;
        break;
      }
    }
    if (!later_occurrence && remaining_uses[argument_node] == 0U &&
        !program.nodes[argument_node].retained_root) {
      final_use_releases.push_back(argument_node);
    }
  }
}

void append_final_use_releases(
    std::string &source, std::span<const std::size_t> final_use_releases,
    std::string_view indentation) {
  for (const std::size_t release : final_use_releases) {
    source += indentation;
    source += "bennu_release(&bennu_resources, &bennu_values[" +
              std::to_string(release) + "]);\n";
  }
}

void append_call_node(std::string &source, std::size_t node_index,
                      const RewriteLoweringNode &node,
                      const RewriteLoweringProgram &program,
                      std::vector<std::size_t> &remaining_uses,
                      std::vector<std::size_t> &final_use_releases) {
  complete_lowered_consumer_emission(
      node, program, remaining_uses, final_use_releases);
  append_call_shape_checks(source, node, program, final_use_releases);
  source += node.spreads_tuple
                ? "  if (!bennu_apply_spread(&bennu_resources, "
                : "  if (!bennu_apply(&bennu_resources, ";
  source += c_implementation_name(node.implementation);
  source += ", &bennu_values[" + std::to_string(node_index) + "], ";
  const auto append_argument = [&source, &node, &program](
                                   std::size_t position) {
    if (node.spreads_tuple) {
      source += "&((BennuValue *)bennu_values[" +
                std::to_string(node.spread_operand) +
                "].data)[" + std::to_string(position) + "]";
    } else {
      source += "&bennu_values[" +
                std::to_string(program.arguments[
                    node.first_argument + position]) +
                "]";
    }
  };
  if (node.argument_count == 0U) {
    source += "NULL, NULL";
  } else {
    append_argument(0U);
    source += ", ";
    if (node.argument_count == 2U) {
      append_argument(1U);
    } else {
      source += "NULL";
    }
  }
  source += ", ";
  append_c_unsigned(source, node.argument_count);
  source += ", \"";
  source += node.admission_point;
  source += "\", ";
  source += c_primitive_id_name(*node.primitive_id);
  source += ", ";
  if (node.spreads_tuple && node.argument_count == 1U) {
    const RewriteLoweringNode &operand =
        program.nodes[node.spread_operand];
    append_source_span(
        source,
        program.tuple_element_spans[operand.first_element]);
  } else {
    append_source_span(source, node.primary_span);
  }
  source += ", ";
  append_source_span(source, node.source_span);
  if (node.spreads_tuple) {
    append_spread_provenance_arguments(source, node, program);
  }
  source += ")) {\n";
  append_final_use_releases(source, final_use_releases, "    ");
  source += "    goto bennu_failure;\n"
            "  }\n";
  append_final_use_releases(source, final_use_releases, "  ");
}

void append_immutable_borrow_node(
    std::string &source, std::size_t node_index,
    const RewriteLoweringNode &node,
    const RewriteLoweringProgram &program,
    std::vector<std::size_t> &remaining_uses,
    std::vector<std::size_t> &final_use_releases) {
  complete_lowered_consumer_emission(
      node, program, remaining_uses, final_use_releases);
  for (std::size_t position = 0U; position < node.argument_count;
       ++position) {
    const std::size_t argument =
        program.arguments[node.first_argument + position];
    source += "  if (!bennu_value_valid(&bennu_values[" +
              std::to_string(argument) +
              "])) {\n"
              "    bennu_set_failure(&bennu_resources, "
              "BENNU_FAILURE_INTERNAL);\n";
    append_final_use_releases(source, final_use_releases, "    ");
    source += "    goto bennu_failure;\n"
              "  }\n";
  }
  if (node.operation ==
      RewriteLoweringOperation::immutable_borrow_failure) {
    source +=
        "  {\n"
        "    BennuScalar bennu_failure_left = {BENNU_INT, 0U, "
        "INT64_MAX, 0.0};\n"
        "    BennuScalar bennu_failure_right = {BENNU_INT, 0U, 0, "
        "0.0};\n"
        "    BennuScalarSignature bennu_failure_signature = "
        "{1U, {BENNU_INT, BENNU_INT}, BENNU_INT};\n"
        "    bennu_set_failure(&bennu_resources, BENNU_FAILURE_DOMAIN);\n"
        "    bennu_set_domain_context(&bennu_resources, BENNU_IMPL_INC_INT, "
        "bennu_failure_left, bennu_failure_right, 0, 0U, \"";
    source += node.admission_point;
    source += "\", BENNU_PRIMITIVE_INC, bennu_failure_signature, 1U, ";
    append_source_span(source, node.primary_span);
    source += ", ";
    append_source_span(source, node.source_span);
    source +=
        ");\n"
        "  }\n";
  } else {
    source += "  bennu_values[" + std::to_string(node_index) +
              "] = bennu_scalar_int(";
    append_c_unsigned(source, node.argument_count);
    source += ");\n";
  }
  append_final_use_releases(source, final_use_releases, "  ");
  if (node.operation ==
      RewriteLoweringOperation::immutable_borrow_failure) {
    source += "  goto bennu_failure;\n";
  }
}

void append_lowered_rewrite_nodes(std::string &source,
                                  const RewriteLoweringProgram &lowering) {
  std::vector<std::size_t> remaining_uses;
  remaining_uses.reserve(lowering.nodes.size());
  for (const RewriteLoweringNode &node : lowering.nodes) {
    remaining_uses.push_back(node.use_count);
  }
  std::size_t maximum_argument_count = 0U;
  for (const RewriteLoweringNode &node : lowering.nodes) {
    if (node.argument_count > maximum_argument_count) {
      maximum_argument_count = node.argument_count;
    }
  }
  std::vector<std::size_t> final_use_releases;
  final_use_releases.reserve(maximum_argument_count);
  for (std::size_t index = 0U; index < lowering.nodes.size(); ++index) {
    const RewriteLoweringNode &node = lowering.nodes[index];
    if (node.kind == RewriteNodeKind::scalar_literal) {
      append_scalar_node(source, index, node);
    } else if (node.kind == RewriteNodeKind::parameter_reference) {
      append_parameter_node(source, index, node);
    } else if (node.kind == RewriteNodeKind::vector_literal) {
      append_vector_node(source, index, node);
    } else if (node.kind == RewriteNodeKind::tuple_literal) {
      append_tuple_node(source, index, node, lowering, remaining_uses);
    } else if (
        node.operation == RewriteLoweringOperation::immutable_borrow ||
        node.operation ==
            RewriteLoweringOperation::immutable_borrow_failure) {
      append_immutable_borrow_node(
          source, index, node, lowering, remaining_uses,
          final_use_releases);
    } else {
      append_call_node(source, index, node, lowering, remaining_uses,
                       final_use_releases);
    }
    if (node.use_count == 0U && !node.retained_root) {
      source += "  bennu_release(&bennu_resources, &bennu_values[" +
                std::to_string(index) + "]);\n";
    }
  }
}

CEmissionResult emit_rewrite_c_source_impl(
    std::string_view source, const CBackendConfiguration &configuration,
    const RewriteProgram *prepared_program,
    const RewriteLoweringProgram *prepared_lowering) {
  std::optional<Error> configuration_error =
      validate_rewrite_configuration(
          configuration.profile, configuration.limits, "rewrite-emitter");
  if (configuration_error.has_value()) {
    return CEmissionResult{false, {}, std::move(*configuration_error)};
  }
  const bool prepared =
      prepared_program != nullptr && prepared_lowering != nullptr;
  RewriteParseResult parsed = [&]() {
    if (prepared_program != nullptr && prepared_lowering != nullptr) {
      return RewriteParseResult{
          true, *prepared_program,
          RewriteDiagnostic{RewriteParseError::none, {}, {}, {}, {}}};
    }
    return parse_rewrite(source);
  }();
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
  std::optional<RewriteEvaluationDiagnostic> parameter_metadata_diagnostic =
      parameter_metadata_preflight_diagnostic(
          parsed.program.parameter_header.span,
          parameter_metadata_input(parsed.program.parameter_header),
          host_parameter_metadata_representation());
  if (parameter_metadata_diagnostic.has_value()) {
    return CEmissionResult{
        false, {},
        public_error_from_diagnostic(source, *parameter_metadata_diagnostic)};
  }
  RewriteResolutionResult resolution =
      prepared ? RewriteResolutionResult{
                     true, RewriteDiagnostic{RewriteParseError::none, {}, {},
                                             {}, {}}}
               : resolve_rewrite_primitives(parsed.program);
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
  std::optional<RewriteEvaluationDiagnostic> profile_diagnostic =
      tuple_profile_diagnostic(parsed.program, configuration.profile);
  if (profile_diagnostic.has_value()) {
    return CEmissionResult{
        false, {},
        public_error_from_diagnostic(source, *profile_diagnostic)};
  }

  RewriteLoweringResult lowered = [&]() {
    if (prepared_program != nullptr && prepared_lowering != nullptr) {
      std::optional<RewriteLoweringProgram> cloned =
          clone_rewrite_lowering_program(*prepared_lowering);
      if (!cloned.has_value()) {
        RewriteEvaluationDiagnostic diagnostic =
            empty_rewrite_evaluation_diagnostic();
        diagnostic.stage = RewriteEvaluationStage::resource_admission;
        diagnostic.error = make_error(
            ErrorKind::resource_error, SourceLocation{1U, 1U, 1U},
            "prepared typed rewrite lowering clone failed");
        return RewriteLoweringResult{false, {}, std::move(diagnostic)};
      }
      return RewriteLoweringResult{
          true, std::move(*cloned), empty_rewrite_evaluation_diagnostic()};
    }
    return lower_rewrite_program(parsed.program);
  }();
  if (!lowered.ok) {
    return CEmissionResult{
        false, {}, public_error_from_diagnostic(source, lowered.diagnostic)};
  }
  if (prepared &&
      !rewrite_lowering_invariants_hold(parsed.program, lowered.program)) {
    return CEmissionResult{
        false, {},
        make_error(
            ErrorKind::invalid_primitive_table,
            SourceLocation{1U, 1U, 1U},
            "prepared typed rewrite lowering violates flat-program "
            "invariants")};
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
    release_evaluation_resources(validation_resources);
    return CEmissionResult{false, {}, std::move(resource_validation.error)};
  }
  const RewriteLoweringProgram &lowering = lowered.program;
  for (const RewriteLoweringNode &node : lowering.nodes) {
    if (node.operation == RewriteLoweringOperation::prepared_value) {
      release_evaluation_resources(validation_resources);
      return CEmissionResult{
          false, {},
          make_error(
              ErrorKind::invalid_primitive_table, node.source_location,
              "prepared owned values cannot be embedded in generated C")};
    }
  }

  std::string generated;
  append_rewrite_c_runtime(generated);
  append_literal_arrays(generated, lowering);
  append_argument_adapter(generated, parsed.program);
  generated += "static int bennu_execute(BennuResources *snapshot) {\n";
  append_resource_initialization(generated, configuration);
  generated += "  (void)bennu_literal;\n"
               "  (void)bennu_tuple;\n"
               "  (void)bennu_apply;\n"
               "  (void)bennu_apply_spread;\n"
               "  (void)bennu_require_shape;\n"
               "  (void)bennu_require_spread_shape;\n"
               "  (void)bennu_source_location;\n"
               "  (void)bennu_source_span;\n"
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
    generated += "    if (snapshot != NULL) { *snapshot = bennu_resources; }\n"
                 "    (void)bennu_report_failure(&bennu_resources);\n"
                 "    return 1;\n";
  }
  generated += "  }\n";

  append_lowered_rewrite_nodes(generated, lowering);
  for (const std::size_t root : lowering.roots) {
    generated += "  if (!bennu_print_value(&bennu_values[" +
                 std::to_string(root) +
                 "])) { goto bennu_output_failure; }\n";
  }
  if (!lowering.nodes.empty()) {
    generated += "  { size_t bennu_index = ";
    append_c_unsigned(generated, lowering.nodes.size());
    generated +=
        ";\n"
        "    while (bennu_index != 0U) {\n"
        "      --bennu_index;\n"
        "      bennu_release(&bennu_resources, &bennu_values[bennu_index]);\n"
        "    }\n"
        "  }\n";
  }
  generated += "  if (snapshot != NULL) { *snapshot = bennu_resources; }\n"
               "  return fflush(stdout) == 0 ? 0 : 1;\n";
  if (!lowering.nodes.empty()) {
    generated += "bennu_failure:\n"
                 "  { size_t bennu_index = ";
    append_c_unsigned(generated, lowering.nodes.size());
    generated += ";\n"
                 "    while (bennu_index != 0U) {\n"
                 "      --bennu_index;\n"
                 "      bennu_release(&bennu_resources, &bennu_values[bennu_index]);\n"
                 "    }\n"
                 "  }\n"
                 "  if (snapshot != NULL) { *snapshot = bennu_resources; }\n"
                 "  (void)bennu_report_failure(&bennu_resources);\n"
                 "  return 1;\n";
    if (!lowering.roots.empty()) {
      generated += "bennu_output_failure:\n"
                   "  { size_t bennu_index = ";
      append_c_unsigned(generated, lowering.nodes.size());
      generated +=
          ";\n"
          "    while (bennu_index != 0U) {\n"
          "      --bennu_index;\n"
          "      bennu_release(&bennu_resources, &bennu_values[bennu_index]);\n"
          "    }\n"
          "  }\n"
          "  if (snapshot != NULL) { *snapshot = bennu_resources; }\n"
          "  (void)fputs(\"OutputError: stdout failure\\n\", stderr);\n"
          "  return 1;\n";
    }
  }
  generated += "}\n\n"
               "#ifndef BENNU_CUSTOM_MAIN\n"
               "int main(int argc, char **argv) {\n"
               "  if (!bennu_bind_arguments(argc, argv)) {\n"
               "    return 1;\n"
               "  }\n"
               "  return bennu_execute(NULL);\n"
               "}\n"
               "#endif\n";
  release_evaluation_resources(validation_resources);
  return CEmissionResult{
      true, std::move(generated),
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U})};
}

#ifndef DOCTEST_CONFIG_DISABLE
CEmissionResult emit_prepared_rewrite_c_source(
    const RewriteProgram &program, const RewriteLoweringProgram &lowering,
    const CBackendConfiguration &configuration) {
  return emit_rewrite_c_source_impl(program.source, configuration, &program,
                                    &lowering);
}
#endif

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
  std::vector<std::uint8_t> seen_roots(program.nodes.size(),
                                       std::uint8_t{0U});
  for (const std::size_t root : program.roots) {
    if (root >= program.nodes.size() ||
        seen_roots[root] != std::uint8_t{0U} ||
        program.nodes[root].span.begin.offset < previous_root_offset) {
      return false;
    }
    seen_roots[root] = std::uint8_t{1U};
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

struct SharedLivenessProbe {
  std::size_t logical_releases;
  std::vector<const void *> released_storage;
};

struct SharedReleaseOrderProbe {
  std::vector<std::size_t> logical_release_ordinals;
};

struct PreparedSharedRewriteFixture {
  RewriteProgram program;
  RewriteLoweringProgram lowering;
};

void configure_prepared_immutable_borrow(
    RewriteLoweringNode &node, RewriteLoweringOperation operation) {
  node.operation = operation;
  node.cardinality = RewriteCardinality::scalar;
  node.element_type = ScalarType::integer;
  node.element_count = 1U;
  node.runtime_shape_check = false;
  node.structural_type = make_scalar_type(ScalarType::integer);
}

PreparedSharedRewriteFixture make_prepared_shared_fixture(
    std::string_view source) {
  RewriteParseResult parsed = parse_rewrite(source);
  (void)resolve_rewrite_primitives(parsed.program);
  parsed.program.arguments[1U] = parsed.program.arguments[0U];
  parsed.program.argument_spans[1U] =
      parsed.program.nodes[parsed.program.arguments[0U]].span;
  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  return PreparedSharedRewriteFixture{
      std::move(parsed.program), std::move(lowered.program)};
}

PreparedSharedRewriteFixture make_prepared_shared_vector_fixture() {
  return make_prepared_shared_fixture("inc[(1 2)]\ninc[0]");
}

PreparedSharedRewriteFixture make_prepared_shared_scalar_fixture() {
  return make_prepared_shared_fixture("inc[41]\ninc[0]");
}

PreparedSharedRewriteFixture make_prepared_shared_empty_vector_fixture() {
  return make_prepared_shared_fixture("inc[Int()]\ninc[0]");
}

PreparedSharedRewriteFixture make_prepared_shared_domain_failure_fixture() {
  RewriteParseResult parsed = parse_rewrite(
      "equals[(9223372036854775807 1) "
      "(9223372036854775807 1)]\ninc[0]");
  (void)resolve_rewrite_primitives(parsed.program);
  const std::size_t first_root = parsed.program.roots[0U];
  const std::size_t second_root = parsed.program.roots[1U];
  const RewriteCall &first_call =
      parsed.program.calls[parsed.program.nodes[first_root].call_index];
  const RewriteCall &second_call =
      parsed.program.calls[parsed.program.nodes[second_root].call_index];
  const std::size_t shared =
      parsed.program.arguments[first_call.first_argument];
  parsed.program.arguments[second_call.first_argument] = shared;
  parsed.program.argument_spans[second_call.first_argument] =
      parsed.program.nodes[shared].span;
  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  return PreparedSharedRewriteFixture{
      std::move(parsed.program), std::move(lowered.program)};
}

PreparedSharedRewriteFixture make_prepared_shared_shape_failure_fixture() {
  RewriteParseResult parsed =
      parse_rewrite("inc[iota[2]]\nadd[iota[1] iota[1]]");
  (void)resolve_rewrite_primitives(parsed.program);
  const std::size_t first_root = parsed.program.roots[0U];
  const std::size_t second_root = parsed.program.roots[1U];
  const RewriteCall &first_call =
      parsed.program.calls[parsed.program.nodes[first_root].call_index];
  const RewriteCall &second_call =
      parsed.program.calls[parsed.program.nodes[second_root].call_index];
  const std::size_t shared =
      parsed.program.arguments[first_call.first_argument];
  parsed.program.arguments[second_call.first_argument + 1U] = shared;
  parsed.program.argument_spans[second_call.first_argument + 1U] =
      parsed.program.nodes[shared].span;
  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  return PreparedSharedRewriteFixture{
      std::move(parsed.program), std::move(lowered.program)};
}

void recompute_prepared_liveness(PreparedSharedRewriteFixture &fixture) {
  fixture.lowering.arguments = fixture.program.arguments;
  fixture.lowering.roots = fixture.program.roots;
  fixture.lowering.tuple_elements = fixture.program.tuple_elements;
  for (RewriteLoweringNode &node : fixture.lowering.nodes) {
    node.use_count = 0U;
    node.retained_root = false;
  }
  for (const std::size_t element : fixture.lowering.tuple_elements) {
    ++fixture.lowering.nodes[element].use_count;
  }
  for (const std::size_t argument : fixture.lowering.arguments) {
    ++fixture.lowering.nodes[argument].use_count;
  }
  for (const std::size_t root : fixture.lowering.roots) {
    ++fixture.lowering.nodes[root].use_count;
    fixture.lowering.nodes[root].retained_root = true;
  }
}

PreparedSharedRewriteFixture make_prepared_shared_tuple_fixture(
    bool fail_second_consumer) {
  RewriteParseResult parsed =
      parse_rewrite("[(1 2) (3 4)]\ninc[0]\ninc[0]");
  (void)resolve_rewrite_primitives(parsed.program);
  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);

  const std::size_t tuple_node = parsed.program.roots[0U];
  const std::size_t first_consumer = parsed.program.roots[1U];
  const std::size_t second_consumer = parsed.program.roots[2U];
  const RewriteCall &first_call =
      parsed.program.calls[parsed.program.nodes[first_consumer].call_index];
  const RewriteCall &second_call =
      parsed.program.calls[parsed.program.nodes[second_consumer].call_index];
  parsed.program.arguments[first_call.first_argument] = tuple_node;
  parsed.program.arguments[second_call.first_argument] = tuple_node;
  parsed.program.argument_spans[first_call.first_argument] =
      parsed.program.nodes[tuple_node].span;
  parsed.program.argument_spans[second_call.first_argument] =
      parsed.program.nodes[tuple_node].span;
  parsed.program.roots = {first_consumer, second_consumer};

  lowered.program.arguments = parsed.program.arguments;
  lowered.program.roots = parsed.program.roots;
  configure_prepared_immutable_borrow(
      lowered.program.nodes[first_consumer],
      RewriteLoweringOperation::immutable_borrow);
  configure_prepared_immutable_borrow(
      lowered.program.nodes[second_consumer],
      fail_second_consumer
          ? RewriteLoweringOperation::immutable_borrow_failure
          : RewriteLoweringOperation::immutable_borrow);
  PreparedSharedRewriteFixture fixture{
      std::move(parsed.program), std::move(lowered.program)};
  recompute_prepared_liveness(fixture);
  return fixture;
}

void record_shared_liveness_event(void *context,
                                  ResourceLifetimeEvent event) {
  if (event.kind != ResourceLifetimeEventKind::logical_release) {
    return;
  }
  SharedLivenessProbe &probe = *static_cast<SharedLivenessProbe *>(context);
  ++probe.logical_releases;
  probe.released_storage.push_back(event.storage);
}

void record_shared_release_order(void *context,
                                 ResourceLifetimeEvent event) {
  if (event.kind != ResourceLifetimeEventKind::logical_release ||
      !event.allocation_ordinal.has_value()) {
    return;
  }
  SharedReleaseOrderProbe &probe =
      *static_cast<SharedReleaseOrderProbe *>(context);
  probe.logical_release_ordinals.push_back(
      *event.allocation_ordinal);
}

std::string make_shared_native_release_probe(
    std::string_view generated,
    std::optional<std::string_view> expected_failure,
    std::span<const std::size_t> expected_release_order) {
  std::string probe =
      "#include <stddef.h>\n"
      "static void *bennu_probe_malloc(size_t size);\n"
      "static void bennu_probe_free(void *data);\n"
      "#define BENNU_RUNTIME_MALLOC(size) bennu_probe_malloc(size)\n"
      "#define BENNU_RUNTIME_FREE(data) bennu_probe_free(data)\n"
      "#define BENNU_CUSTOM_MAIN\n";
  probe.append(generated);
  probe +=
      "\nstatic void *bennu_probe_allocations[16] = {0};\n"
      "static size_t bennu_probe_allocation_count = 0U;\n"
      "static size_t bennu_probe_release_order[16] = {0U};\n"
      "static size_t bennu_probe_release_count = 0U;\n"
      "static size_t bennu_probe_live_count = 0U;\n"
      "static int bennu_probe_invalid_free = 0;\n"
      "static void *bennu_probe_malloc(size_t size) {\n"
      "  void *data = malloc(size);\n"
      "  if (data != NULL) {\n"
      "    if (bennu_probe_allocation_count < 16U) {\n"
      "      bennu_probe_allocations[bennu_probe_allocation_count] = data;\n"
      "    }\n"
      "    ++bennu_probe_allocation_count;\n"
      "    ++bennu_probe_live_count;\n"
      "  }\n"
      "  return data;\n"
      "}\n"
      "static void bennu_probe_free(void *data) {\n"
      "  size_t allocation = bennu_probe_allocation_count < 16U\n"
      "                          ? bennu_probe_allocation_count : 16U;\n"
      "  if (data != NULL) {\n"
      "    while (allocation != 0U) {\n"
      "      --allocation;\n"
      "      if (bennu_probe_allocations[allocation] == data) {\n"
      "        if (bennu_probe_release_count < 16U) {\n"
      "          bennu_probe_release_order[bennu_probe_release_count] = "
      "allocation;\n"
      "        }\n"
      "        bennu_probe_allocations[allocation] = NULL;\n"
      "        ++bennu_probe_release_count;\n"
      "        break;\n"
      "      }\n"
      "    }\n"
      "    if (bennu_probe_live_count == 0U) {\n"
      "      bennu_probe_invalid_free = 1;\n"
      "    } else {\n"
      "      --bennu_probe_live_count;\n"
      "    }\n"
      "  }\n"
      "  free(data);\n"
      "}\n"
      "int main(void) {\n"
      "  BennuResources snapshot = {0};\n"
      "  size_t index = 0U;\n"
      "  if (bennu_execute(&snapshot) ";
  probe += expected_failure.has_value() ? "== 0" : "!= 0";
  probe += " || snapshot.failure != ";
  probe += expected_failure.value_or("BENNU_FAILURE_NONE");
  probe +=
      " || snapshot.live_bytes != 0U || bennu_probe_live_count != 0U ||\n"
      "      bennu_probe_invalid_free != 0 || bennu_probe_release_count != ";
  probe += std::to_string(expected_release_order.size());
  probe += "U) { return 1; }\n"
           "  {\n"
           "    static const size_t expected[] = {";
  for (std::size_t index = 0U; index < expected_release_order.size();
       ++index) {
    if (index != 0U) {
      probe += ", ";
    }
    probe += std::to_string(expected_release_order[index]);
    probe += "U";
  }
  probe +=
      "};\n"
      "    for (index = 0U; index < bennu_probe_release_count; ++index) {\n"
      "      if (bennu_probe_release_order[index] != expected[index]) {\n"
      "        return 1;\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "  return 0;\n"
      "}\n";
  return probe;
}

bool error_value_type_equal(const ErrorValueType &left,
                            const ErrorValueType &right) {
  return structural_type_equal(left, right);
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
  if (kind == RewriteNodeKind::tuple_literal) {
    return "tuple_literal";
  }
  if (kind == RewriteNodeKind::parameter_reference) {
    return "parameter_reference";
  }
  if (kind == RewriteNodeKind::unresolved_name) {
    return "unresolved_name";
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
  if (!program.tuple_elements.empty() ||
      !program.tuple_element_spans.empty()) {
    snapshot.append(";tuple_elements=[");
    for (std::size_t index = 0U; index < program.tuple_elements.size();
         ++index) {
      if (index != 0U) {
        snapshot.push_back(',');
      }
      append_size(snapshot, program.tuple_elements[index]);
    }
    snapshot.append("];tuple_element_spans=[");
    for (std::size_t index = 0U;
         index < program.tuple_element_spans.size(); ++index) {
      if (index != 0U) {
        snapshot.push_back(',');
      }
      append_span(snapshot, program.tuple_element_spans[index]);
    }
    snapshot.push_back(']');
  }
  return snapshot;
}

std::string_view lowering_cardinality_name(RewriteCardinality cardinality) {
  if (cardinality == RewriteCardinality::scalar) {
    return "scalar";
  }
  if (cardinality == RewriteCardinality::static_vector) {
    return "static_vector";
  }
  if (cardinality == RewriteCardinality::tuple) {
    return "tuple";
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
    if (node.spreads_tuple) {
      snapshot.append("/spread=");
      append_size(snapshot, node.spread_operand);
    }
    snapshot.append("/uses=");
    append_size(snapshot, node.use_count);
    snapshot.append("/retained_root=");
    snapshot.push_back(node.retained_root ? '1' : '0');
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
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=5-10,"
        "1:parameter_reference/integer/scalar(1)/impl=0/parameter=42/"
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=16-17,"
        "2:primitive_call/integer/dynamic_vector(0)/impl=9/parameter=-/"
        "arguments=0+1/uses=1/retained_root=0/shape_check=0/span=11-18,"
        "3:primitive_call/integer/dynamic_vector(2)/impl=3/parameter=-/"
        "arguments=1+2/uses=1/retained_root=1/shape_check=1/span=1-19,"
        "4:parameter_reference/integer/scalar(1)/impl=0/parameter=7/"
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=24-25,"
        "5:parameter_reference/double_precision/scalar(1)/impl=0/parameter=8/"
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=26-29,"
        "6:primitive_call/double_precision/scalar(1)/impl=4/parameter=-/"
        "arguments=3+2/uses=1/retained_root=1/shape_check=0/span=20-30]");

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

TEST_CASE("PARG-004-PARAMETER-LOWERING-METADATA") {
  RewriteParseResult parsed =
      parse_rewrite("parameters[n Int]\niota[n]");
  REQUIRE(parsed.ok);
  REQUIRE(parsed.program.parameter_header.present);
  REQUIRE(parsed.program.parameter_header.declarations.size() == 1U);
  CHECK(span_is(parsed.program.parameter_header.span, 1U, 1U, 1U, 18U, 1U,
                18U));
  CHECK(span_is(parsed.program.parameter_header.declarations[0].span, 12U, 1U,
                12U, 17U, 1U, 17U));
  CHECK(span_is(parsed.program.parameter_header.declarations[0].name_span, 12U,
                1U, 12U, 13U, 1U, 13U));
  REQUIRE(resolve_rewrite_primitives(parsed.program).ok);
  REQUIRE(parsed.program.nodes.size() == 2U);
  const RewriteNode &reference = parsed.program.nodes[0];
  CHECK(reference.kind == RewriteNodeKind::parameter_reference);
  CHECK(reference.element_type == ScalarType::integer);
  CHECK(reference.first_element == 0U);
  CHECK(span_is(reference.span, 24U, 2U, 6U, 25U, 2U, 7U));
  CHECK(span_is(reference.declaration_name_span, 12U, 1U, 12U, 13U, 1U,
                13U));

  const RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  REQUIRE(lowered.ok);
  REQUIRE(lowered.program.nodes.size() == 2U);
  const RewriteLoweringNode &lowered_reference = lowered.program.nodes[0];
  CHECK(lowered_reference.kind == RewriteNodeKind::parameter_reference);
  CHECK(lowered_reference.parameter_index == 0U);
  CHECK(lowered_reference.element_type == ScalarType::integer);
  CHECK(lowered_reference.cardinality == RewriteCardinality::scalar);
  CHECK(span_is(lowered_reference.source_span, 24U, 2U, 6U, 25U, 2U, 7U));
  CHECK(span_is(lowered_reference.declaration_name_span, 12U, 1U, 12U, 13U,
                1U, 13U));
}

TEST_CASE("PARG-016-REPRESENTABILITY") {
  const ParameterMetadataRepresentation host =
      host_parameter_metadata_representation();
  const ParameterMetadataPreflightInput ordinary{
      3U, true, 2U, 40U, 7U};
  CHECK(parameter_metadata_preflight(ordinary, host).ok);

  ParameterMetadataRepresentation constrained = host;
  constrained.c_size_maximum =
      static_cast<std::uintmax_t>(std::numeric_limits<int>::max()) - 2U;
  CHECK(parameter_metadata_preflight(
            ParameterMetadataPreflightInput{0U, false, 0U, 0U, 0U},
            constrained)
            .failure ==
        ParameterMetadataPreflightFailure::host_argument_count);

  constrained = host;
  constrained.c_unsigned_literal_maximum = 2U;
  CHECK(parameter_metadata_preflight(
            ParameterMetadataPreflightInput{3U, true, 2U, 2U, 1U},
            constrained)
            .failure ==
        ParameterMetadataPreflightFailure::declaration_count);
  CHECK(parameter_metadata_preflight(
            ParameterMetadataPreflightInput{2U, true, 1U, 2U, 1U},
            constrained)
            .failure ==
        ParameterMetadataPreflightFailure::extra_argument_position);

  CHECK(parameter_metadata_preflight(
            ParameterMetadataPreflightInput{3U, true, 3U, 40U, 7U}, host)
            .failure ==
        ParameterMetadataPreflightFailure::parameter_index);

  constrained = host;
  constrained.c_unsigned_literal_maximum = 39U;
  CHECK(parameter_metadata_preflight(ordinary, constrained).failure ==
        ParameterMetadataPreflightFailure::source_coordinate);

  CHECK(parameter_metadata_preflight(
            ParameterMetadataPreflightInput{
                3U, true, 2U, 40U,
                std::numeric_limits<std::size_t>::max()},
            host)
            .failure ==
        ParameterMetadataPreflightFailure::parameter_name_bytes);

  constrained = host;
  constrained.value_slot_bytes =
      std::numeric_limits<std::uintmax_t>::max();
  CHECK(parameter_metadata_preflight(ordinary, constrained).failure ==
        ParameterMetadataPreflightFailure::value_slots);
  constrained = host;
  constrained.name_table_slot_bytes =
      std::numeric_limits<std::uintmax_t>::max();
  CHECK(parameter_metadata_preflight(ordinary, constrained).failure ==
        ParameterMetadataPreflightFailure::name_table);
  constrained = host;
  constrained.type_table_slot_bytes =
      std::numeric_limits<std::uintmax_t>::max();
  CHECK(parameter_metadata_preflight(ordinary, constrained).failure ==
        ParameterMetadataPreflightFailure::type_table);
  constrained = host;
  constrained.source_span_bytes =
      std::numeric_limits<std::uintmax_t>::max();
  CHECK(parameter_metadata_preflight(ordinary, constrained).failure ==
        ParameterMetadataPreflightFailure::span_table);

  RewriteParseResult parsed = parse_rewrite("parameters[n Int]\n");
  REQUIRE(parsed.ok);
  const ParameterMetadataPreflightInput one_past{
      std::numeric_limits<std::size_t>::max(),
      true,
      std::numeric_limits<std::size_t>::max() - 1U,
      parsed.program.parameter_header.span.end.offset,
      1U};
  std::optional<RewriteEvaluationDiagnostic> diagnostic =
      parameter_metadata_preflight_diagnostic(
          parsed.program.parameter_header.span, one_past, host);
  REQUIRE(diagnostic.has_value());
  Error error =
      public_error_from_diagnostic(parsed.program.source, *diagnostic);
  CHECK(error.kind == ErrorKind::resource_error);
  REQUIRE(error.resource.has_value());
  CHECK(error.resource->reason == ResourceErrorReason::size_overflow);
  CHECK(error.resource->requested_elements ==
        std::optional<std::size_t>{
            std::numeric_limits<std::size_t>::max()});
  CHECK(error.resource->profile == "c-emitter-parameter-metadata");
  CHECK(error.location.offset ==
        parsed.program.parameter_header.span.begin.offset);
  REQUIRE(error.primary_span.has_value());
  CHECK(error.primary_span->begin.offset ==
        parsed.program.parameter_header.span.begin.offset);
  CHECK(error.primary_span->end.offset ==
        parsed.program.parameter_header.span.end.offset);
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

#ifndef DOCTEST_CONFIG_DISABLE
TEST_CASE("SHARED-001 static liveness borrows scalar vector empty-vector and tuple nodes") {
  RewriteParseResult parsed = parse_rewrite("1");
  REQUIRE(parsed.ok);
  REQUIRE(resolve_rewrite_primitives(parsed.program).ok);
  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  REQUIRE(lowered.ok);
  REQUIRE(lowered.program.nodes.size() == 1U);
  RewriteLoweringNode shared_node = std::move(lowered.program.nodes[0]);
  shared_node.use_count = 2U;
  shared_node.retained_root = false;
  const std::array<RewriteLoweringNode, 1> nodes{{std::move(shared_node)}};
  const std::array<std::size_t, 1> arguments{{0U}};

  SUBCASE("scalar borrow remains valid through the final consumer") {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    std::vector<Value> values;
    values.push_back(make_int_value(41));
    std::vector<std::uint8_t> live{std::uint8_t{1U}};
    std::vector<std::size_t> remaining{2U};

    for (std::size_t attempt = 0U; attempt < 2U; ++attempt) {
      REQUIRE(validate_value(values[0]).ok);
      const std::array<const Value *, 1> borrowed{{&values[0]}};
      PrimitiveApplicationContext context{resources, 0U};
      PrimitiveApplicationResult applied = apply_typed_primitive(
          context, *find_primitive(PrimitiveId::inc),
          PrimitiveImplementation::inc_integer, borrowed,
          SourceLocation{1U, 1U, 1U});
      REQUIRE(applied.ok);
      CHECK(applied.value.scalar.integer == 42);
      REQUIRE(complete_rewrite_consumer_attempt(
          resources, arguments, nodes, remaining, values, live));
      CHECK(remaining[0] == 1U - attempt);
      CHECK(live[0] ==
            (attempt == 0U ? std::uint8_t{1U} : std::uint8_t{0U}));
      destroy_value(applied.value);
    }
    CHECK(resources.live_evaluation_bytes == 0U);
  }

  SUBCASE("nonempty vector payload is shared and released exactly once") {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    SharedLivenessProbe probe{0U, {}};
    REQUIRE(set_evaluation_resource_lifetime_observer(
        resources,
        ResourceLifetimeObserver{&probe, &record_shared_liveness_event}));
    const std::array<std::int64_t, 2> elements{{4, 5}};
    VectorAllocationResult allocated = copy_int_vector(
        resources, elements, SourceLocation{1U, 1U, 1U}, "shared-fixture");
    REQUIRE(allocated.ok);
    const void *const shared_storage = allocated.value.vector.integers.get();
    std::vector<Value> values;
    values.push_back(std::move(allocated.value));
    std::vector<std::uint8_t> live{std::uint8_t{1U}};
    std::vector<std::size_t> remaining{2U};

    for (std::size_t attempt = 0U; attempt < 2U; ++attempt) {
      REQUIRE(values[0].vector.integers.get() == shared_storage);
      const std::array<const Value *, 1> borrowed{{&values[0]}};
      PrimitiveApplicationContext context{resources, 0U};
      PrimitiveApplicationResult applied = apply_typed_primitive(
          context, *find_primitive(PrimitiveId::inc),
          PrimitiveImplementation::inc_integer, borrowed,
          SourceLocation{1U, 1U, 1U});
      REQUIRE(applied.ok);
      REQUIRE(complete_rewrite_consumer_attempt(
          resources, arguments, nodes, remaining, values, live));
      if (attempt == 0U) {
        CHECK(probe.logical_releases == 0U);
        CHECK(values[0].vector.integers.get() == shared_storage);
      }
      CHECK(release_value_reservations(resources, applied.value).ok);
    }
    CHECK(probe.logical_releases == 3U);
    REQUIRE(probe.released_storage.size() == 3U);
    CHECK(probe.released_storage[1] == shared_storage);
    CHECK(resources.live_evaluation_bytes == 0U);
  }

  SUBCASE("empty vector has one owner and no payload charge") {
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    VectorAllocationResult allocated = allocate_vector(
        resources, ScalarType::integer, 0U, 0U,
        SourceLocation{1U, 1U, 1U}, "shared-empty-fixture");
    REQUIRE(allocated.ok);
    std::vector<Value> values;
    values.push_back(std::move(allocated.value));
    std::vector<std::uint8_t> live{std::uint8_t{1U}};
    std::vector<std::size_t> remaining{2U};
    for (std::size_t attempt = 0U; attempt < 2U; ++attempt) {
      REQUIRE(validate_value(values[0]).ok);
      const std::array<const Value *, 1> borrowed{{&values[0]}};
      PrimitiveApplicationContext context{resources, 0U};
      PrimitiveApplicationResult applied = apply_typed_primitive(
          context, *find_primitive(PrimitiveId::inc),
          PrimitiveImplementation::inc_integer, borrowed,
          SourceLocation{1U, 1U, 1U});
      REQUIRE(applied.ok);
      REQUIRE(applied.value.container == ContainerKind::vector);
      CHECK(applied.value.vector.integer_count == 0U);
      REQUIRE(complete_rewrite_consumer_attempt(
          resources, arguments, nodes, remaining, values, live));
      CHECK(release_value_reservations(resources, applied.value).ok);
    }
    CHECK(live[0] == std::uint8_t{0U});
    CHECK(resources.live_evaluation_bytes == 0U);
    CHECK(resources.reservation_ordinal == 0U);
  }

  SUBCASE("prepared tuple producer feeds two successful immutable consumers") {
    PreparedSharedRewriteFixture fixture =
        make_prepared_shared_vector_fixture();
    fixture.lowering.nodes[0U].operation =
        RewriteLoweringOperation::prepared_value;
    configure_prepared_immutable_borrow(
        fixture.lowering.nodes[1U],
        RewriteLoweringOperation::immutable_borrow);
    configure_prepared_immutable_borrow(
        fixture.lowering.nodes[3U],
        RewriteLoweringOperation::immutable_borrow);
    REQUIRE(rewrite_lowering_invariants_hold(
        fixture.program, fixture.lowering));

    EvaluationResources resources =
        make_trusted_local_v2_resources({std::nullopt});
    SharedLivenessProbe probe{0U, {}};
    REQUIRE(set_evaluation_resource_lifetime_observer(
        resources,
        ResourceLifetimeObserver{&probe, &record_shared_liveness_event}));
    const std::array<std::int64_t, 1> element{{9}};
    VectorAllocationResult vector = copy_int_vector(
        resources, element, SourceLocation{1U, 1U, 1U},
        "shared-tuple-child");
    REQUIRE(vector.ok);
    std::array<Value, 1> children{{std::move(vector.value)}};
    TupleConstructionResult tuple = make_tuple_value(
        resources, children, SourceLocation{1U, 1U, 1U},
        "shared-tuple-fixture");
    REQUIRE(tuple.ok);

    PreparedRewriteValues prepared{{}, {}, std::nullopt, 0U};
    for (std::size_t index = 0U; index < fixture.lowering.nodes.size();
         ++index) {
      prepared.values.push_back(make_int_value(0));
      prepared.present.push_back(std::uint8_t{0U});
    }
    prepared.values[0U] = move_value(tuple.value);
    prepared.present[0U] = std::uint8_t{1U};
    RewriteEvaluationResult evaluated =
        evaluate_prepared_rewrite_program(
            fixture.program, fixture.lowering,
            RewriteEvaluationCreationData{
                ExecutionProfile::trusted_local_v2,
                ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt},
                AllocationFailureInjection{std::nullopt}},
            &prepared, &resources);
    REQUIRE(evaluated.ok);
    REQUIRE(evaluated.formatted.size() == 2U);
    CHECK(evaluated.formatted[0] == "1");
    CHECK(evaluated.formatted[1] == "1");
    CHECK(prepared.borrow_consumer_ordinal == 2U);
    CHECK(prepared.present[0U] == std::uint8_t{0U});
    CHECK(probe.logical_releases == 2U);
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
    release_rewrite_evaluation_result(evaluated);

    const CEmissionResult unavailable = emit_prepared_rewrite_c_source(
        fixture.program, fixture.lowering,
        CBackendConfiguration{
            ExecutionProfile::trusted_local_v2,
            ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                           std::nullopt},
            AllocationFailureInjection{std::nullopt},
            AllocationFailureInjection{std::nullopt}});
    REQUIRE_FALSE(unavailable.ok);
    CHECK(unavailable.error.kind == ErrorKind::invalid_primitive_table);
    CHECK(unavailable.error.message.find("cannot be embedded") !=
          std::string::npos);
  }

  SUBCASE("prepared tuple failure consumes final borrow and cleans prior result") {
    PreparedSharedRewriteFixture fixture =
        make_prepared_shared_vector_fixture();
    fixture.lowering.nodes[0U].operation =
        RewriteLoweringOperation::prepared_value;
    configure_prepared_immutable_borrow(
        fixture.lowering.nodes[1U],
        RewriteLoweringOperation::immutable_borrow);
    configure_prepared_immutable_borrow(
        fixture.lowering.nodes[3U],
        RewriteLoweringOperation::immutable_borrow);
    EvaluationResources resources =
        make_trusted_local_v2_resources({std::nullopt});
    SharedLivenessProbe probe{0U, {}};
    REQUIRE(set_evaluation_resource_lifetime_observer(
        resources,
        ResourceLifetimeObserver{&probe, &record_shared_liveness_event}));
    const std::array<std::int64_t, 1> element{{9}};
    VectorAllocationResult vector = copy_int_vector(
        resources, element, SourceLocation{1U, 1U, 1U},
        "shared-tuple-failure-child");
    REQUIRE(vector.ok);
    std::array<Value, 1> children{{std::move(vector.value)}};
    TupleConstructionResult tuple = make_tuple_value(
        resources, children, SourceLocation{1U, 1U, 1U},
        "shared-tuple-failure");
    REQUIRE(tuple.ok);
    PreparedRewriteValues prepared{{}, {}, std::size_t{1U}, 0U};
    for (std::size_t index = 0U; index < fixture.lowering.nodes.size();
         ++index) {
      prepared.values.push_back(make_int_value(0));
      prepared.present.push_back(std::uint8_t{0U});
    }
    prepared.values[0U] = move_value(tuple.value);
    prepared.present[0U] = std::uint8_t{1U};
    RewriteEvaluationResult evaluated =
        evaluate_prepared_rewrite_program(
            fixture.program, fixture.lowering,
            RewriteEvaluationCreationData{
                ExecutionProfile::trusted_local_v2,
                ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt},
                AllocationFailureInjection{std::nullopt}},
            &prepared, &resources);
    REQUIRE_FALSE(evaluated.ok);
    CHECK(evaluated.diagnostic.error.kind == ErrorKind::domain_error);
    CHECK(prepared.borrow_consumer_ordinal == 2U);
    CHECK(probe.logical_releases == 2U);
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
    release_rewrite_evaluation_result(evaluated);
  }

  SUBCASE("invalid prepared use count is rejected before tuple ownership moves") {
    PreparedSharedRewriteFixture fixture =
        make_prepared_shared_vector_fixture();
    fixture.lowering.nodes[0U].operation =
        RewriteLoweringOperation::prepared_value;
    configure_prepared_immutable_borrow(
        fixture.lowering.nodes[1U],
        RewriteLoweringOperation::immutable_borrow);
    configure_prepared_immutable_borrow(
        fixture.lowering.nodes[3U],
        RewriteLoweringOperation::immutable_borrow);
    --fixture.lowering.nodes[0U].use_count;
    REQUIRE_FALSE(rewrite_lowering_invariants_hold(
        fixture.program, fixture.lowering));
    EvaluationResources resources =
        make_trusted_local_v2_resources({std::nullopt});
    std::array<Value, 1> children{{make_int_value(9)}};
    TupleConstructionResult tuple = make_tuple_value(
        resources, children, SourceLocation{1U, 1U, 1U},
        "invalid-shared-tuple-lowering");
    REQUIRE(tuple.ok);
    PreparedRewriteValues prepared{{}, {}, std::nullopt, 0U};
    for (std::size_t index = 0U; index < fixture.lowering.nodes.size();
         ++index) {
      prepared.values.push_back(make_int_value(0));
      prepared.present.push_back(std::uint8_t{0U});
    }
    prepared.values[0U] = move_value(tuple.value);
    prepared.present[0U] = std::uint8_t{1U};
    RewriteEvaluationResult evaluated =
        evaluate_prepared_rewrite_program(
            fixture.program, fixture.lowering,
            RewriteEvaluationCreationData{
                ExecutionProfile::trusted_local_v2,
                ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt},
                AllocationFailureInjection{std::nullopt}},
            &prepared, &resources);
    REQUIRE_FALSE(evaluated.ok);
    CHECK(evaluated.diagnostic.error.kind ==
          ErrorKind::invalid_primitive_table);
    CHECK(prepared.present[0U] == std::uint8_t{1U});
    REQUIRE(validate_value(prepared.values[0U]).ok);
    CHECK(release_value_reservations(resources, prepared.values[0U]).ok);
    release_rewrite_evaluation_result(evaluated);
  }

  SUBCASE("prepared node and call structure is rejected before ownership moves") {
    for (std::size_t mutation = 0U; mutation < 23U; ++mutation) {
      PreparedSharedRewriteFixture fixture =
          make_prepared_shared_vector_fixture();
      fixture.lowering.nodes[0U].operation =
          RewriteLoweringOperation::prepared_value;
      configure_prepared_immutable_borrow(
          fixture.lowering.nodes[3U],
          RewriteLoweringOperation::immutable_borrow);
      if (mutation == 0U) {
        fixture.lowering.nodes[1U].kind =
            RewriteNodeKind::vector_literal;
      } else if (mutation == 1U) {
        ++fixture.lowering.nodes[1U].first_argument;
      } else if (mutation == 2U) {
        --fixture.lowering.nodes[1U].argument_count;
      } else if (mutation == 3U) {
        fixture.program.calls[0U].first_argument =
            fixture.program.arguments.size() + 1U;
        fixture.lowering.nodes[1U].first_argument =
            fixture.program.calls[0U].first_argument;
      } else if (mutation == 4U) {
        fixture.lowering.nodes[0U].element_type =
            ScalarType::double_precision;
      } else if (mutation == 5U) {
        --fixture.lowering.nodes[0U].element_count;
      } else if (mutation == 6U) {
        ++fixture.lowering.nodes[0U].first_element;
      } else if (mutation == 7U) {
        ++fixture.lowering.integer_elements[0U];
      } else if (mutation == 8U) {
        ++fixture.lowering.nodes[2U].integer;
      } else if (mutation == 9U) {
        --fixture.program.nodes[0U].element_count;
      } else if (mutation == 10U) {
        fixture.program.calls[0U].primitive = PrimitiveId::add;
      } else if (mutation == 11U) {
        fixture.lowering.nodes[1U].primitive_id = PrimitiveId::add;
      } else if (mutation == 12U) {
        fixture.lowering.nodes[1U].implementation =
            PrimitiveImplementation::add_integer;
      } else if (mutation == 13U) {
        fixture.lowering.nodes[1U].primary_span =
            fixture.program.nodes[1U].span;
      } else if (mutation == 14U) {
        fixture.lowering.boolean_elements.push_back(std::uint8_t{1U});
      } else if (mutation == 15U) {
        fixture.lowering.nodes[1U].element_type =
            ScalarType::boolean;
      } else if (mutation == 16U) {
        ++fixture.lowering.nodes[1U].element_count;
      } else if (mutation == 17U) {
        fixture.lowering.nodes[1U].runtime_shape_check = true;
      } else if (mutation == 18U) {
        fixture.lowering.nodes[3U].cardinality =
            RewriteCardinality::static_vector;
      } else if (mutation == 19U) {
        ++fixture.lowering.nodes[3U].element_count;
      } else if (mutation == 20U) {
        fixture.lowering.nodes[3U].runtime_shape_check = true;
      } else if (mutation == 21U) {
        fixture.lowering.nodes[3U].element_type =
            ScalarType::boolean;
      } else {
        fixture.lowering.nodes[3U].operation =
            static_cast<RewriteLoweringOperation>(255);
      }
      INFO(mutation);
      REQUIRE_FALSE(rewrite_lowering_invariants_hold(
          fixture.program, fixture.lowering));

      EvaluationResources resources =
          make_trusted_local_v2_resources({std::nullopt});
      const std::array<std::int64_t, 2> owner_elements{{9, 10}};
      VectorAllocationResult owner = copy_int_vector(
          resources, owner_elements, SourceLocation{1U, 1U, 1U},
          "invalid-prepared-structure-owner");
      REQUIRE(owner.ok);
      const void *const owner_storage =
          owner.value.vector.integers.get();
      PreparedRewriteValues prepared{{}, {}, std::nullopt, 0U};
      for (std::size_t index = 0U;
           index < fixture.lowering.nodes.size(); ++index) {
        prepared.values.push_back(make_int_value(0));
        prepared.present.push_back(std::uint8_t{0U});
      }
      prepared.values[0U] = move_value(owner.value);
      prepared.present[0U] = std::uint8_t{1U};
      RewriteEvaluationResult evaluated =
          evaluate_prepared_rewrite_program(
              fixture.program, fixture.lowering,
              RewriteEvaluationCreationData{
                  ExecutionProfile::trusted_local_v2,
                  ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                                 std::nullopt},
                  AllocationFailureInjection{std::nullopt}},
              &prepared, &resources);
      REQUIRE_FALSE(evaluated.ok);
      CHECK(evaluated.diagnostic.error.kind ==
            ErrorKind::invalid_primitive_table);
      CHECK(prepared.present[0U] == std::uint8_t{1U});
      REQUIRE(validate_value(prepared.values[0U]).ok);
      CHECK(prepared.values[0U].vector.integers.get() ==
            owner_storage);
      CHECK(resources.live_evaluation_bytes == 16U);
      CHECK(release_value_reservations(
                resources, prepared.values[0U])
                .ok);
      release_rewrite_evaluation_result(evaluated);
      release_evaluation_resources(resources);

      const CEmissionResult emitted = emit_prepared_rewrite_c_source(
          fixture.program, fixture.lowering,
          CBackendConfiguration{
              ExecutionProfile::trusted_local_v2,
              ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                             std::nullopt},
              AllocationFailureInjection{std::nullopt},
              AllocationFailureInjection{std::nullopt}});
      REQUIRE_FALSE(emitted.ok);
      CHECK(emitted.source.empty());
      CHECK(emitted.error.kind == ErrorKind::invalid_primitive_table);
      CHECK(emitted.error.message.find("flat-program invariants") !=
            std::string::npos);
    }
  }
}

TEST_CASE("SHARED-002 generated C releases a shared argument only at static last use") {
  PreparedSharedRewriteFixture fixture =
      make_prepared_shared_vector_fixture();
  REQUIRE(rewrite_program_invariants_hold(fixture.program));
  REQUIRE(rewrite_lowering_invariants_hold(
      fixture.program, fixture.lowering));
  REQUIRE(fixture.lowering.nodes.size() == 4U);
  const std::size_t shared_node = fixture.lowering.arguments[0];
  CHECK(fixture.lowering.nodes[shared_node].use_count == 2U);
  CHECK_FALSE(fixture.lowering.nodes[shared_node].retained_root);
  CHECK(fixture.lowering.nodes[2].use_count == 0U);

  std::string emitted_nodes;
  append_lowered_rewrite_nodes(emitted_nodes, fixture.lowering);
  const std::string first_apply =
      "bennu_apply(&bennu_resources, BENNU_IMPL_INC_INT, "
      "&bennu_values[1]";
  const std::string second_apply =
      "bennu_apply(&bennu_resources, BENNU_IMPL_INC_INT, "
      "&bennu_values[3]";
  const std::string shared_release =
      "bennu_release(&bennu_resources, &bennu_values[0])";
  const std::size_t first_position = emitted_nodes.find(first_apply);
  const std::size_t second_position = emitted_nodes.find(second_apply);
  const std::size_t release_position = emitted_nodes.find(shared_release);
  const std::size_t success_release_position =
      emitted_nodes.find(shared_release, release_position + 1U);
  const std::size_t failure_goto =
      emitted_nodes.find("goto bennu_failure", second_position);
  REQUIRE(first_position != std::string::npos);
  REQUIRE(second_position != std::string::npos);
  REQUIRE(release_position != std::string::npos);
  REQUIRE(success_release_position != std::string::npos);
  REQUIRE(failure_goto != std::string::npos);
  CHECK(first_position < second_position);
  CHECK(second_position < release_position);
  CHECK(release_position < failure_goto);
  CHECK(failure_goto < success_release_position);
  CHECK(emitted_nodes.find(
            shared_release, success_release_position + 1U) ==
        std::string::npos);
  CHECK(emitted_nodes.find("remaining_uses") == std::string::npos);
  CHECK(emitted_nodes.find("reference_count") == std::string::npos);

  const CEmissionResult full = emit_prepared_rewrite_c_source(
      fixture.program, fixture.lowering,
      CBackendConfiguration{
          ExecutionProfile::bounded_v1,
          ResourceLimits{std::nullopt, std::size_t{48U}, std::nullopt,
                         std::nullopt},
          AllocationFailureInjection{std::nullopt},
          AllocationFailureInjection{std::nullopt}});
  REQUIRE(full.ok);
  CHECK(full.source.find("static int bennu_execute(BennuResources *snapshot)") !=
        std::string::npos);
  CHECK(full.source.find("#ifndef BENNU_CUSTOM_MAIN") !=
        std::string::npos);
  const std::size_t full_first = full.source.find(first_apply);
  const std::size_t full_second = full.source.find(second_apply);
  const std::size_t full_release = full.source.find(shared_release);
  REQUIRE(full_first != std::string::npos);
  REQUIRE(full_second != std::string::npos);
  REQUIRE(full_release != std::string::npos);
  CHECK(full_first < full_second);
  CHECK(full_second < full_release);

  const char *const output_directory =
      std::getenv("BENNU_SHARED_LIVENESS_C_DIR");
  if (output_directory != nullptr) {
    struct NativeCase {
      std::string_view name;
      std::size_t live_limit;
      std::optional<std::size_t> failure_ordinal;
    };
    const std::array<NativeCase, 4> native_cases{{
        {"success", 48U, std::nullopt},
        {"boundary", 47U, std::nullopt},
        {"allocation", 48U, std::size_t{2U}},
        {"precedence", 47U, std::size_t{2U}},
    }};
    for (const NativeCase &native_case : native_cases) {
      const CEmissionResult emitted = emit_prepared_rewrite_c_source(
          fixture.program, fixture.lowering,
          CBackendConfiguration{
              ExecutionProfile::bounded_v1,
              ResourceLimits{std::nullopt, native_case.live_limit,
                             std::nullopt, std::nullopt},
              AllocationFailureInjection{std::nullopt},
              AllocationFailureInjection{native_case.failure_ordinal}});
      REQUIRE(emitted.ok);
      const std::string path =
          std::string(output_directory) + "/" +
          std::string(native_case.name) + ".c";
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      REQUIRE(output.good());
      output.write(emitted.source.data(),
                   static_cast<std::streamsize>(emitted.source.size()));
      output.close();
      CHECK(output.good());
    }
  }
}

TEST_CASE("SHARED-KINDS scalar and empty-vector sharing reaches both production backends") {
  struct SharedKindCase {
    std::string_view name;
    PreparedSharedRewriteFixture (*make_fixture)();
    std::string_view formatted;
    std::size_t expected_work;
  };
  const std::array<SharedKindCase, 2> cases{{
      {"scalar", &make_prepared_shared_scalar_fixture, "42", 2U},
      {"empty-vector", &make_prepared_shared_empty_vector_fixture, "()",
       0U},
  }};
  for (const SharedKindCase &shared_case : cases) {
    INFO(shared_case.name);
    PreparedSharedRewriteFixture fixture = shared_case.make_fixture();
    REQUIRE(rewrite_program_invariants_hold(fixture.program));
    REQUIRE(rewrite_lowering_invariants_hold(
        fixture.program, fixture.lowering));
    const std::size_t first_root = fixture.program.roots[0U];
    const RewriteCall &first_call =
        fixture.program.calls[
            fixture.program.nodes[first_root].call_index];
    const std::size_t shared =
        fixture.program.arguments[first_call.first_argument];
    CHECK(fixture.lowering.nodes[shared].use_count == 2U);
    CHECK_FALSE(fixture.lowering.nodes[shared].retained_root);

    RewriteEvaluationResult evaluated =
        evaluate_prepared_rewrite_program(
            fixture.program, fixture.lowering,
            RewriteEvaluationCreationData{
                ExecutionProfile::trusted_local_v1,
                ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt},
                AllocationFailureInjection{std::nullopt}},
            nullptr, nullptr);
    REQUIRE(evaluated.ok);
    REQUIRE(evaluated.formatted.size() == 2U);
    CHECK(evaluated.formatted[0] == shared_case.formatted);
    CHECK(evaluated.formatted[1] == shared_case.formatted);
    CHECK(evaluated.resources.work_units == shared_case.expected_work);
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
    CHECK(evaluated.resources.reservation_ordinal == 0U);
    release_rewrite_evaluation_result(evaluated);

    const CEmissionResult emitted = emit_prepared_rewrite_c_source(
        fixture.program, fixture.lowering,
        CBackendConfiguration{
            ExecutionProfile::trusted_local_v1,
            ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                           std::nullopt},
            AllocationFailureInjection{std::nullopt},
            AllocationFailureInjection{std::nullopt}});
    REQUIRE(emitted.ok);
    const std::string second_apply =
        "bennu_apply(&bennu_resources, BENNU_IMPL_INC_INT, "
        "&bennu_values[3]";
    const std::string shared_release =
        "bennu_release(&bennu_resources, &bennu_values[" +
        std::to_string(shared) + "])";
    const std::size_t second_position =
        emitted.source.find(second_apply);
    const std::size_t release_position =
        emitted.source.find(shared_release, second_position);
    REQUIRE(second_position != std::string::npos);
    REQUIRE(release_position != std::string::npos);
    CHECK(second_position < release_position);
    CHECK(emitted.source.find("reference_count") == std::string::npos);

    const char *const output_directory =
        std::getenv("BENNU_SHARED_LIVENESS_C_DIR");
    if (output_directory != nullptr) {
      const std::string path =
          std::string(output_directory) + "/" +
          std::string(shared_case.name) + ".c";
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      REQUIRE(output.good());
      output.write(
          emitted.source.data(),
          static_cast<std::streamsize>(emitted.source.size()));
      output.close();
      CHECK(output.good());
    }
  }
}

TEST_CASE("SHARED-ORDER distinct final uses release in reverse argument order") {
  const auto make_fixture = [](std::string_view source) {
    RewriteParseResult parsed = parse_rewrite(source);
    (void)resolve_rewrite_primitives(parsed.program);
    RewriteLoweringResult lowered =
        lower_rewrite_program(parsed.program);
    return PreparedSharedRewriteFixture{
        std::move(parsed.program), std::move(lowered.program)};
  };
  const CBackendConfiguration c_configuration{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{std::nullopt},
      AllocationFailureInjection{std::nullopt}};

  SUBCASE("successful evaluator and generated C release right then left") {
    PreparedSharedRewriteFixture fixture =
        make_fixture("add[(1 2) (3 4)]");
    REQUIRE(rewrite_program_invariants_hold(fixture.program));
    REQUIRE(rewrite_lowering_invariants_hold(
        fixture.program, fixture.lowering));
    SharedReleaseOrderProbe evaluator_probe{{}};
    EvaluationResources evaluator_resources =
        make_trusted_local_resources({std::nullopt});
    REQUIRE(set_evaluation_resource_lifetime_observer(
        evaluator_resources,
        ResourceLifetimeObserver{
            &evaluator_probe, &record_shared_release_order}));
    RewriteEvaluationResult evaluated =
        evaluate_prepared_rewrite_program(
            fixture.program, fixture.lowering,
            RewriteEvaluationCreationData{
                ExecutionProfile::trusted_local_v1,
                ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt},
                AllocationFailureInjection{std::nullopt}},
            nullptr, &evaluator_resources);
    REQUIRE(evaluated.ok);
    REQUIRE(evaluated.formatted.size() == 1U);
    CHECK(evaluated.formatted[0] == "(4 6)");
    CHECK(evaluator_probe.logical_release_ordinals ==
          std::vector<std::size_t>{1U, 0U});
    release_rewrite_evaluation_result(evaluated);
    CHECK(evaluator_probe.logical_release_ordinals ==
          std::vector<std::size_t>{1U, 0U, 2U});

    const CEmissionResult emitted = emit_prepared_rewrite_c_source(
        fixture.program, fixture.lowering, c_configuration);
    REQUIRE(emitted.ok);
    const std::string apply =
        "if (!bennu_apply(&bennu_resources, BENNU_IMPL_ADD_INT";
    const std::string release_right =
        "bennu_release(&bennu_resources, &bennu_values[1])";
    const std::string release_left =
        "bennu_release(&bennu_resources, &bennu_values[0])";
    const std::size_t apply_position = emitted.source.find(apply);
    const std::size_t failure_right =
        emitted.source.find(release_right, apply_position);
    const std::size_t failure_left =
        emitted.source.find(release_left, apply_position);
    const std::size_t failure_goto =
        emitted.source.find("goto bennu_failure", apply_position);
    const std::size_t success_right =
        emitted.source.find(release_right, failure_goto);
    const std::size_t success_left =
        emitted.source.find(release_left, failure_goto);
    REQUIRE(apply_position != std::string::npos);
    REQUIRE(failure_right != std::string::npos);
    REQUIRE(failure_left != std::string::npos);
    REQUIRE(failure_goto != std::string::npos);
    REQUIRE(success_right != std::string::npos);
    REQUIRE(success_left != std::string::npos);
    CHECK(apply_position < failure_right);
    CHECK(failure_right < failure_left);
    CHECK(failure_left < failure_goto);
    CHECK(failure_goto < success_right);
    CHECK(success_right < success_left);

    const char *const output_directory =
        std::getenv("BENNU_SHARED_LIVENESS_C_DIR");
    if (output_directory != nullptr) {
      const std::array<std::size_t, 3> release_order{{1U, 0U, 2U}};
      const std::string probe = make_shared_native_release_probe(
          emitted.source, std::nullopt, release_order);
      const std::string path =
          std::string(output_directory) + "/reverse-success.c";
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      REQUIRE(output.good());
      output.write(probe.data(),
                   static_cast<std::streamsize>(probe.size()));
      output.close();
      CHECK(output.good());
    }
  }

  SUBCASE("real primitive failure releases result then right then left") {
    PreparedSharedRewriteFixture fixture = make_fixture(
        "add[(9223372036854775807 1) (1 2)]");
    REQUIRE(rewrite_program_invariants_hold(fixture.program));
    REQUIRE(rewrite_lowering_invariants_hold(
        fixture.program, fixture.lowering));
    SharedReleaseOrderProbe evaluator_probe{{}};
    EvaluationResources evaluator_resources =
        make_trusted_local_resources({std::nullopt});
    REQUIRE(set_evaluation_resource_lifetime_observer(
        evaluator_resources,
        ResourceLifetimeObserver{
            &evaluator_probe, &record_shared_release_order}));
    RewriteEvaluationResult evaluated =
        evaluate_prepared_rewrite_program(
            fixture.program, fixture.lowering,
            RewriteEvaluationCreationData{
                ExecutionProfile::trusted_local_v1,
                ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt},
                AllocationFailureInjection{std::nullopt}},
            nullptr, &evaluator_resources);
    REQUIRE_FALSE(evaluated.ok);
    CHECK(evaluated.diagnostic.error.kind == ErrorKind::domain_error);
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
    CHECK(evaluator_probe.logical_release_ordinals ==
          std::vector<std::size_t>{2U, 1U, 0U});
    release_rewrite_evaluation_result(evaluated);

    const CEmissionResult emitted = emit_prepared_rewrite_c_source(
        fixture.program, fixture.lowering, c_configuration);
    REQUIRE(emitted.ok);
    const char *const output_directory =
        std::getenv("BENNU_SHARED_LIVENESS_C_DIR");
    if (output_directory != nullptr) {
      const std::array<std::size_t, 3> release_order{{2U, 1U, 0U}};
      const std::string probe = make_shared_native_release_probe(
          emitted.source,
          std::optional<std::string_view>{"BENNU_FAILURE_DOMAIN"},
          release_order);
      const std::string path =
          std::string(output_directory) + "/reverse-domain-failure.c";
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      REQUIRE(output.good());
      output.write(probe.data(),
                   static_cast<std::streamsize>(probe.size()));
      output.close();
      CHECK(output.good());
    }
  }

  SUBCASE("repeated final-use argument releases exactly once") {
    RewriteParseResult parsed = parse_rewrite("1");
    REQUIRE(parsed.ok);
    REQUIRE(resolve_rewrite_primitives(parsed.program).ok);
    RewriteLoweringResult lowered =
        lower_rewrite_program(parsed.program);
    REQUIRE(lowered.ok);
    lowered.program.nodes[0U].use_count = 2U;
    lowered.program.nodes[0U].retained_root = false;
    const std::array<std::size_t, 2> duplicate_arguments{{0U, 0U}};
    std::vector<std::size_t> remaining_uses{2U};
    EvaluationResources resources =
        make_trusted_local_resources({std::nullopt});
    SharedReleaseOrderProbe probe{{}};
    REQUIRE(set_evaluation_resource_lifetime_observer(
        resources,
        ResourceLifetimeObserver{&probe, &record_shared_release_order}));
    const std::array<std::int64_t, 2> elements{{1, 2}};
    VectorAllocationResult vector = copy_int_vector(
        resources, elements, SourceLocation{1U, 1U, 1U},
        "duplicate-final-use");
    REQUIRE(vector.ok);
    std::vector<Value> values;
    values.push_back(std::move(vector.value));
    std::vector<std::uint8_t> live{std::uint8_t{1U}};
    REQUIRE(complete_rewrite_consumer_attempt(
        resources, duplicate_arguments, lowered.program.nodes,
        remaining_uses, values, live));
    CHECK(remaining_uses[0U] == 0U);
    CHECK(live[0U] == std::uint8_t{0U});
    CHECK(probe.logical_release_ordinals ==
          std::vector<std::size_t>{0U});
    CHECK(resources.live_evaluation_bytes == 0U);
    release_evaluation_resources(resources);
  }
}

TEST_CASE("SHARED-FAILURE production C completes final uses before cleanup") {
  struct FailureCase {
    std::string_view name;
    PreparedSharedRewriteFixture (*make_fixture)();
    ErrorKind evaluator_error;
    std::string_view c_failure;
    std::array<std::size_t, 4> release_order;
    bool shape_failure;
  };
  const std::array<FailureCase, 2> cases{{
      {"production-domain-failure",
       &make_prepared_shared_domain_failure_fixture,
       ErrorKind::domain_error, "BENNU_FAILURE_DOMAIN", {1U, 3U, 0U, 2U},
       false},
      {"production-shape-failure",
       &make_prepared_shared_shape_failure_fixture,
       ErrorKind::shape_mismatch, "BENNU_FAILURE_SHAPE", {3U, 0U, 2U, 1U},
       true},
  }};
  for (const FailureCase &failure_case : cases) {
    INFO(failure_case.name);
    PreparedSharedRewriteFixture fixture = failure_case.make_fixture();
    REQUIRE(rewrite_program_invariants_hold(fixture.program));
    REQUIRE(rewrite_lowering_invariants_hold(
        fixture.program, fixture.lowering));
    const std::size_t first_root = fixture.program.roots[0U];
    const RewriteCall &first_call =
        fixture.program.calls[
            fixture.program.nodes[first_root].call_index];
    const std::size_t shared =
        fixture.program.arguments[first_call.first_argument];
    CHECK(fixture.lowering.nodes[shared].use_count == 2U);

    SharedReleaseOrderProbe evaluator_probe{{}};
    EvaluationResources evaluator_resources =
        make_trusted_local_resources({std::nullopt});
    REQUIRE(set_evaluation_resource_lifetime_observer(
        evaluator_resources,
        ResourceLifetimeObserver{
            &evaluator_probe, &record_shared_release_order}));
    RewriteEvaluationResult evaluated =
        evaluate_prepared_rewrite_program(
            fixture.program, fixture.lowering,
            RewriteEvaluationCreationData{
                ExecutionProfile::trusted_local_v1,
                ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt},
                AllocationFailureInjection{std::nullopt}},
            nullptr, &evaluator_resources);
    REQUIRE_FALSE(evaluated.ok);
    CHECK(evaluated.diagnostic.error.kind ==
          failure_case.evaluator_error);
    CHECK(evaluated.resources.live_evaluation_bytes == 0U);
    CHECK(evaluator_probe.logical_release_ordinals ==
          std::vector<std::size_t>(
              failure_case.release_order.begin(),
              failure_case.release_order.end()));
    release_rewrite_evaluation_result(evaluated);

    const CEmissionResult emitted = emit_prepared_rewrite_c_source(
        fixture.program, fixture.lowering,
        CBackendConfiguration{
            ExecutionProfile::trusted_local_v1,
            ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                           std::nullopt},
            AllocationFailureInjection{std::nullopt},
            AllocationFailureInjection{std::nullopt}});
    REQUIRE(emitted.ok);
    const std::string shared_release =
        "bennu_release(&bennu_resources, &bennu_values[" +
        std::to_string(shared) + "])";
    const std::size_t failure_call =
        failure_case.shape_failure
            ? emitted.source.find("if (!bennu_require_shape")
            : emitted.source.find(
                  "if (!bennu_apply(&bennu_resources, "
                  "BENNU_IMPL_INC_INT");
    const std::size_t failure_release =
        emitted.source.find(shared_release, failure_call);
    const std::size_t failure_goto =
        emitted.source.find("goto bennu_failure", failure_call);
    REQUIRE(failure_call != std::string::npos);
    REQUIRE(failure_release != std::string::npos);
    REQUIRE(failure_goto != std::string::npos);
    CHECK(failure_call < failure_release);
    CHECK(failure_release < failure_goto);
    CHECK(emitted.source.find("reference_count") == std::string::npos);

    const char *const output_directory =
        std::getenv("BENNU_SHARED_LIVENESS_C_DIR");
    if (output_directory != nullptr) {
      const std::string probe = make_shared_native_release_probe(
          emitted.source,
          std::optional<std::string_view>{failure_case.c_failure},
          failure_case.release_order);
      const std::string path =
          std::string(output_directory) + "/" +
          std::string(failure_case.name) + ".c";
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      REQUIRE(output.good());
      output.write(probe.data(),
                   static_cast<std::streamsize>(probe.size()));
      output.close();
      CHECK(output.good());
    }
  }
}

TEST_CASE("SHARED-TUPLE prepared tuple sharing is evaluator and C equivalent") {
  const RewriteEvaluationCreationData exact_creation{
      ExecutionProfile::bounded_v2,
      ResourceLimits{std::nullopt, std::size_t{64U}, std::nullopt,
                     std::size_t{32U}},
      AllocationFailureInjection{std::nullopt}};
  PreparedSharedRewriteFixture success =
      make_prepared_shared_tuple_fixture(false);
  REQUIRE(rewrite_program_invariants_hold(success.program));
  REQUIRE(rewrite_lowering_invariants_hold(
      success.program, success.lowering));
  const std::size_t tuple_node = success.program.tuple_elements.size();
  REQUIRE(tuple_node < success.lowering.nodes.size());
  CHECK(success.lowering.nodes[tuple_node].use_count == 2U);
  CHECK_FALSE(success.lowering.nodes[tuple_node].retained_root);

  SharedLivenessProbe success_probe{0U, {}};
  EvaluationResources success_resources =
      make_evaluation_resources(
          exact_creation.profile, exact_creation.limits,
          exact_creation.allocation_failure, 0U, 0U, 0U);
  REQUIRE(set_evaluation_resource_lifetime_observer(
      success_resources,
      ResourceLifetimeObserver{
          &success_probe, &record_shared_liveness_event}));
  RewriteEvaluationResult evaluated =
      evaluate_prepared_rewrite_program(
          success.program, success.lowering, exact_creation, nullptr,
          &success_resources);
  REQUIRE(evaluated.ok);
  REQUIRE(evaluated.formatted.size() == 2U);
  CHECK(evaluated.formatted[0] == "1");
  CHECK(evaluated.formatted[1] == "1");
  CHECK(evaluated.scalar_kernel_invocations == 0U);
  CHECK(evaluated.resources.reservation_ordinal == 3U);
  CHECK(evaluated.resources.live_evaluation_bytes == 0U);
  CHECK(success_probe.logical_releases == 3U);
  release_rewrite_evaluation_result(evaluated);

  CEmissionResult emitted = emit_prepared_rewrite_c_source(
      success.program, success.lowering,
      CBackendConfiguration{
          exact_creation.profile, exact_creation.limits,
          AllocationFailureInjection{std::nullopt},
          AllocationFailureInjection{std::nullopt}});
  REQUIRE(emitted.ok);
  const std::string first_borrow =
      "bennu_values[4] = bennu_scalar_int(1U)";
  const std::string second_borrow =
      "bennu_values[6] = bennu_scalar_int(1U)";
  const std::string tuple_release =
      "bennu_release(&bennu_resources, &bennu_values[2])";
  const std::size_t first_borrow_position =
      emitted.source.find(first_borrow);
  const std::size_t second_borrow_position =
      emitted.source.find(second_borrow);
  const std::size_t second_validation_position =
      emitted.source.find(
          "if (!bennu_value_valid(&bennu_values[2]))",
          first_borrow_position + first_borrow.size());
  const std::size_t tuple_release_position =
      emitted.source.find(tuple_release, second_validation_position);
  const std::size_t tuple_success_release_position =
      emitted.source.find(tuple_release, tuple_release_position + 1U);
  const std::size_t tuple_failure_goto =
      emitted.source.find("goto bennu_failure",
                          second_validation_position);
  REQUIRE(first_borrow_position != std::string::npos);
  REQUIRE(second_borrow_position != std::string::npos);
  REQUIRE(second_validation_position != std::string::npos);
  REQUIRE(tuple_release_position != std::string::npos);
  REQUIRE(tuple_success_release_position != std::string::npos);
  REQUIRE(tuple_failure_goto != std::string::npos);
  CHECK(first_borrow_position < second_validation_position);
  CHECK(second_validation_position < tuple_release_position);
  CHECK(tuple_release_position < tuple_failure_goto);
  CHECK(tuple_failure_goto < second_borrow_position);
  CHECK(second_borrow_position < tuple_success_release_position);
  CHECK(emitted.source.find(
            tuple_release, tuple_success_release_position + 1U) ==
        std::string::npos);
  CHECK(emitted.source.find("reference_count") == std::string::npos);

  SUBCASE("later consumer failure releases the shared tuple and prior root") {
    PreparedSharedRewriteFixture failure =
        make_prepared_shared_tuple_fixture(true);
    SharedLivenessProbe failure_probe{0U, {}};
    EvaluationResources failure_resources =
        make_evaluation_resources(
            exact_creation.profile, exact_creation.limits,
            exact_creation.allocation_failure, 0U, 0U, 0U);
    REQUIRE(set_evaluation_resource_lifetime_observer(
        failure_resources,
        ResourceLifetimeObserver{
            &failure_probe, &record_shared_liveness_event}));
    RewriteEvaluationResult failed =
        evaluate_prepared_rewrite_program(
            failure.program, failure.lowering, exact_creation, nullptr,
            &failure_resources);
    REQUIRE_FALSE(failed.ok);
    CHECK(failed.diagnostic.error.kind == ErrorKind::domain_error);
    CHECK(failed.scalar_kernel_invocations == 0U);
    CHECK(failed.resources.reservation_ordinal == 3U);
    CHECK(failed.resources.live_evaluation_bytes == 0U);
    CHECK(failure_probe.logical_releases == 3U);
    release_rewrite_evaluation_result(failed);
  }

  SUBCASE("live-byte and allocation precedence is deterministic") {
    struct FailureCase {
      std::size_t live_limit;
      std::optional<std::size_t> failure_ordinal;
      ResourceErrorReason reason;
      std::size_t reservation_ordinal;
    };
    const std::array<FailureCase, 5> cases{{
        {63U, std::nullopt, ResourceErrorReason::profile_limit, 2U},
        {64U, std::size_t{0U},
         ResourceErrorReason::allocation_unavailable, 1U},
        {64U, std::size_t{1U},
         ResourceErrorReason::allocation_unavailable, 2U},
        {64U, std::size_t{2U},
         ResourceErrorReason::allocation_unavailable, 3U},
        {63U, std::size_t{2U},
         ResourceErrorReason::profile_limit, 2U},
    }};
    for (const FailureCase &failure_case : cases) {
      RewriteEvaluationCreationData creation{
          ExecutionProfile::bounded_v2,
          ResourceLimits{
              std::nullopt, failure_case.live_limit, std::nullopt,
              std::size_t{32U}},
          AllocationFailureInjection{failure_case.failure_ordinal}};
      RewriteEvaluationResult failed =
          evaluate_prepared_rewrite_program(
              success.program, success.lowering, creation, nullptr,
              nullptr);
      INFO(failure_case.live_limit);
      INFO(failure_case.failure_ordinal);
      REQUIRE_FALSE(failed.ok);
      CHECK(failed.diagnostic.error.kind == ErrorKind::resource_error);
      REQUIRE(failed.diagnostic.error.resource.has_value());
      CHECK(failed.diagnostic.error.resource->reason ==
            failure_case.reason);
      CHECK(failed.resources.reservation_ordinal ==
            failure_case.reservation_ordinal);
      CHECK(failed.resources.live_evaluation_bytes == 0U);
      CHECK(failed.values.empty());
      release_rewrite_evaluation_result(failed);
    }
  }

  SUBCASE("root retention defers tuple release to result cleanup") {
    PreparedSharedRewriteFixture retained =
        make_prepared_shared_tuple_fixture(false);
    retained.program.roots.insert(
        retained.program.roots.begin(), tuple_node);
    retained.lowering.roots = retained.program.roots;
    ++retained.lowering.nodes[tuple_node].use_count;
    retained.lowering.nodes[tuple_node].retained_root = true;
    REQUIRE(rewrite_lowering_invariants_hold(
        retained.program, retained.lowering));
    RewriteEvaluationResult retained_result =
        evaluate_prepared_rewrite_program(
            retained.program, retained.lowering, exact_creation, nullptr,
            nullptr);
    REQUIRE(retained_result.ok);
    REQUIRE(retained_result.formatted.size() == 3U);
    CHECK(retained_result.formatted[0] == "[(1 2) (3 4)]");
    CHECK(retained_result.formatted[1] == "1");
    CHECK(retained_result.formatted[2] == "1");
    CHECK(retained_result.resources.live_evaluation_bytes == 64U);
    release_rewrite_evaluation_result(retained_result);
  }

  SUBCASE("invalid prepared ownership is rejected before allocation") {
    PreparedSharedRewriteFixture invalid =
        make_prepared_shared_tuple_fixture(false);
    --invalid.lowering.nodes[tuple_node].use_count;
    REQUIRE_FALSE(rewrite_lowering_invariants_hold(
        invalid.program, invalid.lowering));
    RewriteEvaluationResult rejected =
        evaluate_prepared_rewrite_program(
            invalid.program, invalid.lowering, exact_creation, nullptr,
            nullptr);
    REQUIRE_FALSE(rejected.ok);
    CHECK(rejected.diagnostic.error.kind ==
          ErrorKind::invalid_primitive_table);
    CHECK(rejected.resources.reservation_ordinal == 0U);
    CHECK(rejected.resources.live_evaluation_bytes == 0U);
    CEmissionResult rejected_c = emit_prepared_rewrite_c_source(
        invalid.program, invalid.lowering,
        CBackendConfiguration{
            exact_creation.profile, exact_creation.limits,
            AllocationFailureInjection{std::nullopt},
            AllocationFailureInjection{std::nullopt}});
    CHECK_FALSE(rejected_c.ok);
    CHECK(rejected_c.error.kind == ErrorKind::invalid_primitive_table);
  }

  SUBCASE("tuple children reject every alias before prepared ownership moves") {
    for (std::size_t mutation = 0U; mutation < 3U; ++mutation) {
      PreparedSharedRewriteFixture invalid =
          make_prepared_shared_tuple_fixture(false);
      const std::size_t child = invalid.program.tuple_elements[0U];
      invalid.lowering.nodes[child].operation =
          RewriteLoweringOperation::prepared_value;
      if (mutation == 0U) {
        const std::size_t later_root = invalid.program.roots[1U];
        const RewriteCall &later_call =
            invalid.program.calls[
                invalid.program.nodes[later_root].call_index];
        invalid.program.arguments[later_call.first_argument] = child;
        invalid.program.argument_spans[later_call.first_argument] =
            invalid.program.nodes[child].span;
      } else if (mutation == 1U) {
        invalid.program.roots.insert(
            invalid.program.roots.begin(), child);
      } else {
        invalid.program.tuple_elements[1U] = child;
      }
      recompute_prepared_liveness(invalid);
      INFO(mutation);
      REQUIRE_FALSE(rewrite_lowering_invariants_hold(
          invalid.program, invalid.lowering));

      EvaluationResources resources =
          make_trusted_local_v2_resources({std::nullopt});
      const std::array<std::int64_t, 2> elements{{1, 2}};
      VectorAllocationResult owner = copy_int_vector(
          resources, elements, SourceLocation{1U, 1U, 1U},
          "exclusive-tuple-child");
      REQUIRE(owner.ok);
      const void *const owner_storage = owner.value.vector.integers.get();
      PreparedRewriteValues prepared{{}, {}, std::nullopt, 0U};
      for (std::size_t index = 0U;
           index < invalid.lowering.nodes.size(); ++index) {
        prepared.values.push_back(make_int_value(0));
        prepared.present.push_back(std::uint8_t{0U});
      }
      prepared.values[child] = move_value(owner.value);
      prepared.present[child] = std::uint8_t{1U};

      RewriteEvaluationResult rejected =
          evaluate_prepared_rewrite_program(
              invalid.program, invalid.lowering, exact_creation,
              &prepared, &resources);
      REQUIRE_FALSE(rejected.ok);
      CHECK(rejected.diagnostic.error.kind ==
            ErrorKind::invalid_primitive_table);
      CHECK(prepared.present[child] == std::uint8_t{1U});
      REQUIRE(validate_value(prepared.values[child]).ok);
      CHECK(prepared.values[child].vector.integers.get() ==
            owner_storage);
      CHECK(resources.reservation_ordinal == 1U);
      CHECK(resources.live_evaluation_bytes == 16U);

      const CEmissionResult rejected_c =
          emit_prepared_rewrite_c_source(
              invalid.program, invalid.lowering,
              CBackendConfiguration{
                  exact_creation.profile, exact_creation.limits,
                  AllocationFailureInjection{std::nullopt},
                  AllocationFailureInjection{std::nullopt}});
      REQUIRE_FALSE(rejected_c.ok);
      CHECK(rejected_c.source.empty());
      CHECK(rejected_c.error.kind ==
            ErrorKind::invalid_primitive_table);
      CHECK(rejected_c.error.message.find("flat-program invariants") !=
            std::string::npos);

      CHECK(release_value_reservations(
                resources, prepared.values[child])
                .ok);
      release_evaluation_resources(resources);
      release_rewrite_evaluation_result(rejected);
    }
  }

  const char *const output_directory =
      std::getenv("BENNU_SHARED_TUPLE_C_DIR");
  if (output_directory != nullptr) {
    struct NativeCase {
      std::string_view name;
      bool fail_second_consumer;
      std::size_t live_limit;
      std::optional<std::size_t> failure_ordinal;
    };
    const std::array<NativeCase, 5> native_cases{{
        {"tuple-success", false, 64U, std::nullopt},
        {"tuple-consumer-failure", true, 64U, std::nullopt},
        {"tuple-boundary", false, 63U, std::nullopt},
        {"tuple-allocation", false, 64U, std::size_t{2U}},
        {"tuple-precedence", false, 63U, std::size_t{2U}},
    }};
    for (const NativeCase &native_case : native_cases) {
      PreparedSharedRewriteFixture fixture =
          make_prepared_shared_tuple_fixture(
              native_case.fail_second_consumer);
      CEmissionResult native = emit_prepared_rewrite_c_source(
          fixture.program, fixture.lowering,
          CBackendConfiguration{
              ExecutionProfile::bounded_v2,
              ResourceLimits{
                  std::nullopt, native_case.live_limit, std::nullopt,
                  std::size_t{32U}},
              AllocationFailureInjection{std::nullopt},
              AllocationFailureInjection{
                  native_case.failure_ordinal}});
      REQUIRE(native.ok);
      const std::string path =
          std::string(output_directory) + "/" +
          std::string(native_case.name) + ".c";
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      REQUIRE(output.good());
      output.write(native.source.data(),
                   static_cast<std::streamsize>(native.source.size()));
      output.close();
      CHECK(output.good());
    }
  }
}

TEST_CASE("SHARED-ROOT duplicate owned roots are rejected before either backend") {
  RewriteParseResult parsed = parse_rewrite("(1 2)");
  REQUIRE(parsed.ok);
  REQUIRE(resolve_rewrite_primitives(parsed.program).ok);
  REQUIRE(parsed.program.roots.size() == 1U);
  parsed.program.roots.push_back(parsed.program.roots[0]);
  CHECK_FALSE(rewrite_program_invariants_hold(parsed.program));
  const RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  REQUIRE_FALSE(lowered.ok);
  CHECK(lowered.diagnostic.stage == RewriteEvaluationStage::primitive_table);
  CHECK(lowered.diagnostic.error.kind == ErrorKind::invalid_primitive_table);
}

TEST_CASE("SHARED-003 failure cleanup and live-byte boundaries are deterministic") {
  const std::array<std::int64_t, 2> elements{{1, 2}};
  RewriteParseResult parsed = parse_rewrite("1");
  REQUIRE(parsed.ok);
  REQUIRE(resolve_rewrite_primitives(parsed.program).ok);
  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  REQUIRE(lowered.ok);
  RewriteLoweringNode shared_node = std::move(lowered.program.nodes[0]);
  shared_node.use_count = 2U;
  shared_node.retained_root = false;
  const std::array<RewriteLoweringNode, 1> nodes{{std::move(shared_node)}};
  const std::array<std::size_t, 1> argument_nodes{{0U}};

  const auto run_boundary =
      [&elements, &nodes, &argument_nodes](
          std::size_t live_limit,
          AllocationFailureInjection allocation_failure) {
        EvaluationResources resources = make_bounded_resources(
            ResourceLimits{std::nullopt, live_limit, std::nullopt,
                           std::nullopt},
            allocation_failure);
        SharedLivenessProbe probe{0U, {}};
        CHECK(set_evaluation_resource_lifetime_observer(
            resources,
            ResourceLifetimeObserver{&probe, &record_shared_liveness_event}));
        VectorAllocationResult input = copy_int_vector(
            resources, elements, SourceLocation{1U, 1U, 1U},
            "shared-boundary");
        REQUIRE(input.ok);
        std::vector<Value> values;
        values.push_back(std::move(input.value));
        values.push_back(make_int_value(0));
        std::vector<std::uint8_t> live{
            std::uint8_t{1U}, std::uint8_t{0U}};
        std::vector<std::size_t> remaining{2U};
        const std::array<const Value *, 1> borrowed{{&values[0]}};

        PrimitiveApplicationContext first_context{resources, 0U};
        PrimitiveApplicationResult first = apply_typed_primitive(
            first_context, *find_primitive(PrimitiveId::inc),
            PrimitiveImplementation::inc_integer, borrowed,
            SourceLocation{1U, 1U, 1U});
        REQUIRE(first.ok);
        REQUIRE(complete_rewrite_consumer_attempt(
            resources, argument_nodes, nodes, remaining, values, live));
        values[1] = std::move(first.value);
        live[1] = std::uint8_t{1U};
        CHECK(resources.live_evaluation_bytes == 32U);

        PrimitiveApplicationContext second_context{
            resources, first_context.scalar_kernel_invocations};
        PrimitiveApplicationResult second = apply_typed_primitive(
            second_context, *find_primitive(PrimitiveId::inc),
            PrimitiveImplementation::inc_integer, borrowed,
            SourceLocation{2U, 2U, 1U});
        REQUIRE(complete_rewrite_consumer_attempt(
            resources, argument_nodes, nodes, remaining, values, live));
        CHECK(live[0] == std::uint8_t{0U});
        if (second.ok) {
          CHECK(resources.live_evaluation_bytes == 32U);
          CHECK(release_value_reservations(resources, second.value).ok);
        }
        release_rewrite_node_values(resources, values, live);
        CHECK(resources.live_evaluation_bytes == 0U);
        release_evaluation_resources(resources);
        return std::pair<bool, std::size_t>{second.ok,
                                            probe.logical_releases};
      };

  const auto refused = run_boundary(47U, {std::nullopt});
  CHECK_FALSE(refused.first);
  CHECK(refused.second == 2U);
  const auto admitted = run_boundary(48U, {std::nullopt});
  CHECK(admitted.first);
  CHECK(admitted.second == 3U);
  const auto injected = run_boundary(
      48U, AllocationFailureInjection{std::size_t{2U}});
  CHECK_FALSE(injected.first);
  CHECK(injected.second == 2U);
}

TEST_CASE("SHARED-004 prepared flat graph runs through the production evaluator") {
  const auto run = [](std::size_t live_limit,
                      AllocationFailureInjection failure) {
    PreparedSharedRewriteFixture fixture =
        make_prepared_shared_vector_fixture();
    return evaluate_prepared_rewrite_program(
        fixture.program, fixture.lowering,
        RewriteEvaluationCreationData{
            ExecutionProfile::bounded_v1,
            ResourceLimits{std::nullopt, live_limit, std::nullopt,
                           std::nullopt},
            failure},
        nullptr, nullptr);
  };

  RewriteEvaluationResult exact =
      run(48U, AllocationFailureInjection{std::nullopt});
  REQUIRE(exact.ok);
  REQUIRE(exact.formatted.size() == 2U);
  CHECK(exact.formatted[0] == "(2 3)");
  CHECK(exact.formatted[1] == "(2 3)");
  CHECK(exact.resources.live_evaluation_bytes == 32U);
  CHECK(exact.resources.reservation_ordinal == 3U);
  release_rewrite_evaluation_result(exact);
  CHECK(exact.resources.live_evaluation_bytes == 0U);
  CHECK(exact.resources.owner.token == 0U);

  RewriteEvaluationResult boundary =
      run(47U, AllocationFailureInjection{std::nullopt});
  REQUIRE_FALSE(boundary.ok);
  REQUIRE(boundary.diagnostic.error.resource.has_value());
  CHECK(boundary.diagnostic.error.resource->reason ==
        ResourceErrorReason::profile_limit);
  CHECK(boundary.diagnostic.error.resource->limit_kind ==
        ResourceLimitKind::max_live_evaluation_bytes);
  CHECK(boundary.diagnostic.error.resource->configured_limit ==
        std::optional<std::size_t>{47U});
  CHECK(boundary.resources.live_evaluation_bytes == 0U);
  CHECK(boundary.resources.reservation_ordinal == 2U);
  release_rewrite_evaluation_result(boundary);

  RewriteEvaluationResult injected =
      run(48U, AllocationFailureInjection{std::size_t{2U}});
  REQUIRE_FALSE(injected.ok);
  REQUIRE(injected.diagnostic.error.resource.has_value());
  CHECK(injected.diagnostic.error.resource->reason ==
        ResourceErrorReason::allocation_unavailable);
  CHECK(injected.diagnostic.error.resource->allocation_ordinal ==
        std::optional<std::size_t>{2U});
  CHECK(injected.resources.live_evaluation_bytes == 0U);
  CHECK(injected.resources.reservation_ordinal == 3U);
  release_rewrite_evaluation_result(injected);

  PreparedSharedRewriteFixture precedence_fixture =
      make_prepared_shared_vector_fixture();
  RewriteEvaluationResult precedence = evaluate_prepared_rewrite_program(
      precedence_fixture.program, precedence_fixture.lowering,
      RewriteEvaluationCreationData{
          ExecutionProfile::bounded_v1,
          ResourceLimits{std::nullopt, std::size_t{47U}, std::nullopt,
                         std::nullopt},
          AllocationFailureInjection{std::size_t{2U}}},
      nullptr, nullptr);
  REQUIRE_FALSE(precedence.ok);
  REQUIRE(precedence.diagnostic.error.resource.has_value());
  CHECK(precedence.diagnostic.error.resource->reason ==
        ResourceErrorReason::profile_limit);
  CHECK(precedence.resources.reservation_ordinal == 2U);
  CHECK(precedence.resources.live_evaluation_bytes == 0U);
  release_rewrite_evaluation_result(precedence);
}
#endif

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

TEST_CASE("TUP-050 invalid configuration precedes source analysis") {
  struct PrecedenceCase {
    RewriteEvaluationCreationData creation;
    std::string_view expected_message;
  };
  const ResourceLimits no_limits{
      std::nullopt, std::nullopt, std::nullopt, std::nullopt};
  const ResourceLimits tuple_limit{
      std::nullopt, std::nullopt, std::nullopt, 16U};
  const std::array<PrecedenceCase, 3> cases{{
      {{ExecutionProfile::bounded_v2, no_limits,
        AllocationFailureInjection{std::nullopt}},
       "bounded-v2 requires at least one configured resource limit"},
      {{ExecutionProfile::trusted_local_v1, tuple_limit,
        AllocationFailureInjection{std::nullopt}},
       "trusted-local-v1 requires every resource limit to be omitted"},
      {{ExecutionProfile::bounded_v1, tuple_limit,
        AllocationFailureInjection{std::nullopt}},
       "bounded-v1 does not support max_tuple_table_bytes"},
  }};
  const std::array<std::string_view, 3> sources{{
      "add[1, 2]",
      "[[1]]",
      "bogus[1]",
  }};

  for (const PrecedenceCase &precedence_case : cases) {
    for (const std::string_view source : sources) {
      INFO(precedence_case.expected_message);
      INFO(source);
      RewriteEvaluationResult evaluated =
          evaluate_rewrite_source(source, precedence_case.creation);
      REQUIRE_FALSE(evaluated.ok);
      CHECK(evaluated.diagnostic.stage ==
            RewriteEvaluationStage::resource_admission);
      CHECK(evaluated.diagnostic.error.kind ==
            ErrorKind::invalid_execution_profile);
      CHECK(evaluated.diagnostic.error.static_message ==
            precedence_case.expected_message);
      REQUIRE(evaluated.diagnostic.error.primitive.has_value());
      CHECK(evaluated.diagnostic.error.primitive->name ==
            "rewrite-evaluator");
      CHECK(evaluated.resources.owner.token == 0U);
      CHECK(evaluated.resources.reservation_ordinal == 0U);
      CHECK(evaluated.resources.live_evaluation_bytes == 0U);

      const CBackendConfiguration emitter_configuration{
          precedence_case.creation.profile,
          precedence_case.creation.limits,
          AllocationFailureInjection{std::nullopt},
          AllocationFailureInjection{std::nullopt}};
      CEmissionResult emitted =
          emit_rewrite_c_source_impl(source, emitter_configuration, nullptr,
                                     nullptr);
      REQUIRE_FALSE(emitted.ok);
      CHECK(emitted.source.empty());
      CHECK(emitted.error.kind == ErrorKind::invalid_execution_profile);
      CHECK(emitted.error.static_message ==
            precedence_case.expected_message);
      REQUIRE(emitted.error.primitive.has_value());
      CHECK(emitted.error.primitive->name == "rewrite-emitter");
      CHECK(emitted.error.location.offset == 1U);
      CHECK(emitted.error.location.line == 1U);
      CHECK(emitted.error.location.column == 1U);
    }
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
  CHECK(evaluated.resources.owner.token == 0U);
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
  CHECK(evaluated.resources.owner.token == 0U);
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
  CHECK(released.resources.owner.token == 0U);

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

TEST_CASE("TUP-001-GRAMMAR") {
  const RewriteParseResult comprehensive = parse_rewrite(
      "parameters[n Int]\n"
      "[]\n"
      "[n]\n"
      "[1 2.5 true [n]]\n"
      "[\n"
      "  n\n"
      "]\n"
      "add[]\n"
      "[]");
  REQUIRE(comprehensive.ok);
  CHECK(rewrite_flat_snapshot(comprehensive.program) ==
        R"snapshot(roots=[0,2,8,10,11,12];nodes=[{kind=tuple_literal,span=[19:2:1,21:2:3),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=parameter_reference,span=[23:3:2,24:3:3),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[22:3:1,25:3:4),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=1,first_element_span=0,call_index=0},{kind=scalar_literal,span=[27:4:2,28:4:3),element_type=integer,boolean=0,integer=1,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=scalar_literal,span=[29:4:4,32:4:7),element_type=double_precision,boolean=0,integer=0,double_precision=bits:4004000000000000,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=scalar_literal,span=[33:4:8,37:4:12),element_type=boolean,boolean=1,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=parameter_reference,span=[39:4:14,40:4:15),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[38:4:13,41:4:16),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=1,element_count=1,first_element_span=1,call_index=0},{kind=tuple_literal,span=[26:4:1,42:4:17),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=2,element_count=4,first_element_span=2,call_index=0},{kind=parameter_reference,span=[47:6:3,48:6:4),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[43:5:1,50:7:2),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=6,element_count=1,first_element_span=6,call_index=0},{kind=primitive_call,span=[51:8:1,56:8:6),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[57:9:1,59:9:3),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=7,element_count=0,first_element_span=7,call_index=0}];arguments=[];argument_spans=[];calls=[{syntax=bracketed,name=add,name_span=[51:8:1,54:8:4),opening_delimiter_span=[54:8:4,55:8:5),closing_delimiter_span=[55:8:5,56:8:6),prefix_separator_span=[55:8:5,55:8:5),span=[51:8:1,56:8:6),first_argument=0,argument_count=0,primitive=none}];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[];tuple_elements=[1,6,3,4,5,7,9];tuple_element_spans=[[23:3:2,24:3:3),[39:4:14,40:4:15),[27:4:2,28:4:3),[29:4:4,32:4:7),[33:4:8,37:4:12),[38:4:13,41:4:16),[47:6:3,48:6:4)])snapshot");

  const std::array<std::string_view, 8> exact_sources{{
      "add[1 2]",
      "add [1 2]",
      "add []",
      "parameters[x Int]\n[x x]",
      "parameters[x Int]\n[inc x]",
      "parameters[x Int]\n[x inc x]",
      "unknown [1 2]",
      " \r\nparameters[\r\n x Int\r\n]\r\n[\r\n x\r\n]\r\n",
  }};
  const std::array<std::string_view, 8> exact_snapshots{{
      R"snapshot(roots=[2];nodes=[{kind=scalar_literal,span=[5:1:5,6:1:6),element_type=integer,boolean=0,integer=1,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=scalar_literal,span=[7:1:7,8:1:8),element_type=integer,boolean=0,integer=2,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=primitive_call,span=[1:1:1,9:1:9),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0}];arguments=[0,1];argument_spans=[[5:1:5,6:1:6),[7:1:7,8:1:8)];calls=[{syntax=bracketed,name=add,name_span=[1:1:1,4:1:4),opening_delimiter_span=[4:1:4,5:1:5),closing_delimiter_span=[8:1:8,9:1:9),prefix_separator_span=[5:1:5,5:1:5),span=[1:1:1,9:1:9),first_argument=0,argument_count=2,primitive=none}];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[])snapshot",
      R"snapshot(roots=[3];nodes=[{kind=scalar_literal,span=[6:1:6,7:1:7),element_type=integer,boolean=0,integer=1,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=scalar_literal,span=[8:1:8,9:1:9),element_type=integer,boolean=0,integer=2,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[5:1:5,10:1:10),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=2,first_element_span=0,call_index=0},{kind=primitive_call,span=[1:1:1,10:1:10),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0}];arguments=[2];argument_spans=[[5:1:5,10:1:10)];calls=[{syntax=prefix,name=add,name_span=[1:1:1,4:1:4),opening_delimiter_span=[4:1:4,4:1:4),closing_delimiter_span=[5:1:5,5:1:5),prefix_separator_span=[4:1:4,5:1:5),span=[1:1:1,10:1:10),first_argument=0,argument_count=1,primitive=none}];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[];tuple_elements=[0,1];tuple_element_spans=[[6:1:6,7:1:7),[8:1:8,9:1:9)])snapshot",
      R"snapshot(roots=[1];nodes=[{kind=tuple_literal,span=[5:1:5,7:1:7),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=primitive_call,span=[1:1:1,7:1:7),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0}];arguments=[0];argument_spans=[[5:1:5,7:1:7)];calls=[{syntax=prefix,name=add,name_span=[1:1:1,4:1:4),opening_delimiter_span=[4:1:4,4:1:4),closing_delimiter_span=[5:1:5,5:1:5),prefix_separator_span=[4:1:4,5:1:5),span=[1:1:1,7:1:7),first_argument=0,argument_count=1,primitive=none}];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[])snapshot",
      R"snapshot(roots=[2];nodes=[{kind=parameter_reference,span=[20:2:2,21:2:3),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=parameter_reference,span=[22:2:4,23:2:5),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[19:2:1,24:2:6),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=2,first_element_span=0,call_index=0}];arguments=[];argument_spans=[];calls=[];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[];tuple_elements=[0,1];tuple_element_spans=[[20:2:2,21:2:3),[22:2:4,23:2:5)])snapshot",
      R"snapshot(roots=[2];nodes=[{kind=parameter_reference,span=[24:2:6,25:2:7),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=primitive_call,span=[20:2:2,25:2:7),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[19:2:1,26:2:8),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=1,first_element_span=0,call_index=0}];arguments=[0];argument_spans=[[24:2:6,25:2:7)];calls=[{syntax=prefix,name=inc,name_span=[20:2:2,23:2:5),opening_delimiter_span=[23:2:5,23:2:5),closing_delimiter_span=[24:2:6,24:2:6),prefix_separator_span=[23:2:5,24:2:6),span=[20:2:2,25:2:7),first_argument=0,argument_count=1,primitive=none}];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[];tuple_elements=[1];tuple_element_spans=[[20:2:2,25:2:7)])snapshot",
      R"snapshot(roots=[3];nodes=[{kind=parameter_reference,span=[20:2:2,21:2:3),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=parameter_reference,span=[26:2:8,27:2:9),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=primitive_call,span=[22:2:4,27:2:9),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[19:2:1,28:2:10),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=2,first_element_span=0,call_index=0}];arguments=[1];argument_spans=[[26:2:8,27:2:9)];calls=[{syntax=prefix,name=inc,name_span=[22:2:4,25:2:7),opening_delimiter_span=[25:2:7,25:2:7),closing_delimiter_span=[26:2:8,26:2:8),prefix_separator_span=[25:2:7,26:2:8),span=[22:2:4,27:2:9),first_argument=0,argument_count=1,primitive=none}];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[];tuple_elements=[0,2];tuple_element_spans=[[20:2:2,21:2:3),[22:2:4,27:2:9)])snapshot",
      R"snapshot(roots=[3];nodes=[{kind=scalar_literal,span=[10:1:10,11:1:11),element_type=integer,boolean=0,integer=1,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=scalar_literal,span=[12:1:12,13:1:13),element_type=integer,boolean=0,integer=2,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[9:1:9,14:1:14),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=2,first_element_span=0,call_index=0},{kind=primitive_call,span=[1:1:1,14:1:14),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0}];arguments=[2];argument_spans=[[9:1:9,14:1:14)];calls=[{syntax=prefix,name=unknown,name_span=[1:1:1,8:1:8),opening_delimiter_span=[8:1:8,8:1:8),closing_delimiter_span=[9:1:9,9:1:9),prefix_separator_span=[8:1:8,9:1:9),span=[1:1:1,14:1:14),first_argument=0,argument_count=1,primitive=none}];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[];tuple_elements=[0,1];tuple_element_spans=[[10:1:10,11:1:11),[12:1:12,13:1:13)])snapshot",
      R"snapshot(roots=[1];nodes=[{kind=parameter_reference,span=[32:6:2,33:6:3),element_type=integer,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=0,first_element_span=0,call_index=0},{kind=tuple_literal,span=[28:5:1,36:7:2),element_type=boolean,boolean=0,integer=0,double_precision=bits:0,first_element=0,element_count=1,first_element_span=0,call_index=0}];arguments=[];argument_spans=[];calls=[];boolean_elements=[];integer_elements=[];double_elements=[];vector_element_spans=[];tuple_elements=[0];tuple_element_spans=[[32:6:2,33:6:3)])snapshot",
  }};
  for (std::size_t index = 0U; index < exact_sources.size(); ++index) {
    const std::string_view source = exact_sources[index];
    const RewriteParseResult exact = parse_rewrite(source);
    REQUIRE(exact.ok);
    INFO(source);
    CHECK(rewrite_flat_snapshot(exact.program) == exact_snapshots[index]);
  }
  RewriteParseResult unknown_prefix = parse_rewrite("unknown [1 2]");
  REQUIRE(unknown_prefix.ok);
  const RewriteResolutionResult unknown_resolution =
      resolve_rewrite_primitives(unknown_prefix.program);
  REQUIRE_FALSE(unknown_resolution.ok);
  CHECK(unknown_resolution.diagnostic.error ==
        RewriteParseError::unknown_primitive);
  CHECK(span_is(unknown_resolution.diagnostic.primary, 1U, 1U, 1U, 8U, 1U,
                8U));
  CHECK(span_is(unknown_resolution.diagnostic.context, 1U, 1U, 1U, 14U, 1U,
                14U));
  CHECK(span_is(unknown_resolution.diagnostic.related, 1U, 1U, 1U, 8U, 1U,
                8U));

  const std::array<std::string_view, 4> lowering_sources{{
      "[]",
      "[1]",
      "[\n 1\n [2.5 true]\n]",
      "parameters[n Int]\n[n]",
  }};
  const std::array<std::string_view, 4> lowering_snapshots{{
      "roots=[0];arguments=[];nodes=["
      "0:tuple_literal/boolean/tuple(0)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=1/shape_check=0/span=1-3]",
      "roots=[1];arguments=[];nodes=["
      "0:scalar_literal/integer/scalar(1)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=2-3,"
      "1:tuple_literal/boolean/tuple(1)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=1/shape_check=0/span=1-4]",
      "roots=[4];arguments=[];nodes=["
      "0:scalar_literal/integer/scalar(1)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=4-5,"
      "1:scalar_literal/double_precision/scalar(1)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=8-11,"
      "2:scalar_literal/boolean/scalar(1)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=12-16,"
      "3:tuple_literal/boolean/tuple(2)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=7-17,"
      "4:tuple_literal/boolean/tuple(2)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=1/shape_check=0/span=1-19]",
      "roots=[1];arguments=[];nodes=["
      "0:parameter_reference/integer/scalar(1)/impl=0/parameter=0/"
      "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=20-21,"
      "1:tuple_literal/boolean/tuple(1)/impl=0/parameter=-/"
      "arguments=0+0/uses=1/retained_root=1/shape_check=0/span=19-22]",
  }};
  for (std::size_t index = 0U; index < lowering_sources.size(); ++index) {
    const std::string_view source = lowering_sources[index];
    RewriteParseResult lowering_parse = parse_rewrite(source);
    REQUIRE(lowering_parse.ok);
    REQUIRE(resolve_rewrite_primitives(lowering_parse.program).ok);
    RewriteLoweringResult lowering_result =
        lower_rewrite_program(lowering_parse.program);
    REQUIRE(lowering_result.ok);
    INFO(source);
    CHECK(rewrite_lowering_snapshot(lowering_result.program) ==
          lowering_snapshots[index]);
  }

  const RewriteParseResult invalid = parse_rewrite("[1, 2]");
  REQUIRE_FALSE(invalid.ok);
  CHECK(invalid.diagnostic.error == RewriteParseError::invalid_byte);
  CHECK(span_is(invalid.diagnostic.primary, 3U, 1U, 3U, 4U, 1U, 4U));
  CHECK(span_is(invalid.diagnostic.context, 1U, 1U, 1U, 7U, 1U, 7U));
  CHECK(span_is(invalid.diagnostic.related, 1U, 1U, 1U, 2U, 1U, 2U));

  const RewriteParseResult missing_separator =
      parse_rewrite("[1[2]]");
  REQUIRE_FALSE(missing_separator.ok);
  CHECK(missing_separator.diagnostic.error ==
        RewriteParseError::missing_separator);
  CHECK(span_is(missing_separator.diagnostic.primary, 3U, 1U, 3U, 4U, 1U,
                4U));
  CHECK(span_is(missing_separator.diagnostic.context, 1U, 1U, 1U, 7U, 1U,
                7U));
  CHECK(span_is(missing_separator.diagnostic.related, 1U, 1U, 1U, 2U, 1U,
                2U));

  const RewriteParseResult missing_delimiter = parse_rewrite("[1");
  REQUIRE_FALSE(missing_delimiter.ok);
  CHECK(missing_delimiter.diagnostic.error ==
        RewriteParseError::missing_delimiter);
  CHECK(span_is(missing_delimiter.diagnostic.primary, 3U, 1U, 3U, 3U, 1U,
                3U));
  CHECK(span_is(missing_delimiter.diagnostic.context, 1U, 1U, 1U, 3U, 1U,
                3U));
  CHECK(span_is(missing_delimiter.diagnostic.related, 1U, 1U, 1U, 2U, 1U,
                2U));

  const RewriteParseResult mismatched = parse_rewrite("[1)");
  REQUIRE_FALSE(mismatched.ok);
  CHECK(mismatched.diagnostic.error ==
        RewriteParseError::mismatched_delimiter);
  CHECK(span_is(mismatched.diagnostic.primary, 3U, 1U, 3U, 4U, 1U, 4U));
  CHECK(span_is(mismatched.diagnostic.context, 1U, 1U, 1U, 4U, 1U, 4U));
  CHECK(span_is(mismatched.diagnostic.related, 1U, 1U, 1U, 2U, 1U, 2U));

  const RewriteParseResult parsed =
      parse_rewrite("[[1 2] add[3 4]]");
  REQUIRE(parsed.ok);
  REQUIRE(parsed.program.roots.size() == 1U);
  REQUIRE(parsed.program.tuple_elements.size() == 4U);
  REQUIRE(parsed.program.tuple_element_spans.size() == 4U);
  const RewriteNode &outer =
      parsed.program.nodes[parsed.program.roots[0]];
  REQUIRE(outer.kind == RewriteNodeKind::tuple_literal);
  CHECK(outer.element_count == 2U);
  CHECK(outer.span.begin.offset == 1U);
  CHECK(outer.span.end.offset == 17U);
  CHECK(parsed.program.tuple_element_spans[2].begin.offset == 2U);
  CHECK(parsed.program.tuple_element_spans[2].end.offset == 7U);
  CHECK(parsed.program.tuple_element_spans[3].begin.offset == 8U);
  CHECK(parsed.program.tuple_element_spans[3].end.offset == 16U);
#ifndef DOCTEST_CONFIG_DISABLE
  const std::string snapshot = rewrite_flat_snapshot(parsed.program);
  CHECK(snapshot.find("tuple_elements=[0,1,2,5]") !=
        std::string::npos);
  CHECK(snapshot.find(
            "tuple_element_spans=[[3:1:3,4:1:4)") !=
        std::string::npos);
#endif

  RewriteParseResult resolved = parsed;
  REQUIRE(resolve_rewrite_primitives(resolved.program).ok);
  RewriteLoweringResult lowered =
      lower_rewrite_program(resolved.program);
  REQUIRE(lowered.ok);
  TypeFormattingResult formatted =
      format_type(lowered.program.nodes[resolved.program.roots[0]]
                      .structural_type);
  REQUIRE(formatted.ok);
  CHECK(formatted.formatted == "Tuple<Tuple<Int, Int>, Int>");
  CHECK(rewrite_lowering_snapshot(lowered.program) ==
        "roots=[6];arguments=[3,4];nodes=["
        "0:scalar_literal/integer/scalar(1)/impl=0/parameter=-/"
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=3-4,"
        "1:scalar_literal/integer/scalar(1)/impl=0/parameter=-/"
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=5-6,"
        "2:tuple_literal/boolean/tuple(2)/impl=0/parameter=-/"
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=2-7,"
        "3:scalar_literal/integer/scalar(1)/impl=0/parameter=-/"
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=12-13,"
        "4:scalar_literal/integer/scalar(1)/impl=0/parameter=-/"
        "arguments=0+0/uses=1/retained_root=0/shape_check=0/span=14-15,"
        "5:primitive_call/integer/scalar(1)/impl=3/parameter=-/"
        "arguments=0+2/uses=1/retained_root=0/shape_check=0/span=8-16,"
        "6:tuple_literal/boolean/tuple(2)/impl=0/parameter=-/"
        "arguments=0+0/uses=1/retained_root=1/shape_check=0/span=1-17]");
}

TEST_CASE("TUP-050-EVALUATOR-FORMAT-PROFILE") {
  const RewriteEvaluationCreationData trusted_v2{
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult values = evaluate_rewrite_source(
      "[]\n[1]\n[1 2.5 true]\n[1 [2 3]]\n[(1 2) add[3 4]]",
      trusted_v2);
  REQUIRE(values.ok);
  REQUIRE(values.formatted.size() == 5U);
  CHECK(values.formatted[0] == "[]");
  CHECK(values.formatted[1] == "[1]");
  CHECK(values.formatted[2] == "[1 2.5 true]");
  CHECK(values.formatted[3] == "[1 [2 3]]");
  CHECK(values.formatted[4] == "[(1 2) 7]");
  CHECK(values.resources.reservation_ordinal == 6U);
  CHECK(values.resources.live_evaluation_bytes == 176U);
  release_rewrite_evaluation_result(values);

  const RewriteEvaluationCreationData bounded_exact{
      ExecutionProfile::bounded_v2,
      ResourceLimits{64U, 64U, 16U, 16U},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult singleton =
      evaluate_rewrite_source("[1]", bounded_exact);
  REQUIRE(singleton.ok);
  CHECK(singleton.resources.live_evaluation_bytes == 16U);
  CHECK(singleton.resources.work_units == 0U);
  release_rewrite_evaluation_result(singleton);

  RewriteEvaluationResult refused =
      evaluate_rewrite_source("[1 2]", bounded_exact);
  REQUIRE_FALSE(refused.ok);
  REQUIRE(refused.diagnostic.error.resource.has_value());
  CHECK(refused.diagnostic.error.resource->limit_kind ==
        ResourceLimitKind::max_tuple_table_bytes);
  CHECK(refused.diagnostic.error.resource->refused_charge == 32U);
  CHECK(refused.resources.live_evaluation_bytes == 0U);
  CHECK(refused.resources.reservation_ordinal == 0U);

  const RewriteEvaluationCreationData v1{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult profile =
      evaluate_rewrite_source("inc[[1]]", v1);
  REQUIRE_FALSE(profile.ok);
  CHECK(profile.diagnostic.error.kind == ErrorKind::profile_error);
  REQUIRE(profile.diagnostic.error.profile.has_value());
  CHECK(profile.diagnostic.error.profile->reason ==
        ProfileErrorReason::unsupported_value_kind);
  CHECK(span_is(profile.diagnostic.primary, 5U, 1U, 5U, 8U, 1U, 8U));
  CHECK(profile.scalar_kernel_invocations == 0U);

  RewriteEvaluationResult nested_profile =
      evaluate_rewrite_source("[[1]]", v1);
  REQUIRE_FALSE(nested_profile.ok);
  CHECK(span_is(nested_profile.diagnostic.primary, 1U, 1U, 1U, 6U, 1U,
                6U));
  CHECK(span_is(nested_profile.diagnostic.context, 1U, 1U, 1U, 6U, 1U,
                6U));
  CHECK(span_is(nested_profile.diagnostic.related, 1U, 1U, 1U, 6U, 1U,
                6U));

  CEmissionResult nested_emission =
      emit_rewrite_c_source_impl(
          "[[1]]",
          c_backend_configuration(EvaluationConfiguration{
              v1.profile, v1.limits, v1.allocation_failure}),
          nullptr, nullptr);
  REQUIRE_FALSE(nested_emission.ok);
  CHECK(nested_emission.error.kind == ErrorKind::profile_error);
  CHECK(nested_emission.error.location.offset == 1U);
  CHECK(nested_emission.error.location.line == 1U);
  CHECK(nested_emission.error.location.column == 1U);
}

TEST_CASE("TUP-050-FAULT-TRANSACTION") {
  for (std::size_t ordinal = 0U; ordinal < 3U; ++ordinal) {
    const RewriteEvaluationCreationData creation{
        ExecutionProfile::trusted_local_v2,
        ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                       std::nullopt},
        AllocationFailureInjection{ordinal}};
    RewriteEvaluationResult failed =
        evaluate_rewrite_source("[(1 2) [3]]", creation);
    INFO(ordinal);
    REQUIRE_FALSE(failed.ok);
    REQUIRE(failed.diagnostic.error.resource.has_value());
    CHECK(failed.diagnostic.error.resource->reason ==
          ResourceErrorReason::allocation_unavailable);
    CHECK(failed.resources.reservation_ordinal == ordinal + 1U);
    CHECK(failed.resources.live_evaluation_bytes == 0U);
    CHECK(failed.values.empty());
    CHECK(failed.formatted.empty());
  }

  const auto record_event = [](void *context, ResourceLifetimeEvent event) {
    auto &events =
        *static_cast<std::vector<ResourceLifetimeEvent> *>(context);
    events.push_back(event);
  };
  std::vector<ResourceLifetimeEvent> success_events;
  const RewriteEvaluationCreationData success_creation{
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{std::nullopt},
      ResourceLifetimeObserver{&success_events, record_event}};
  RewriteEvaluationResult released =
      evaluate_rewrite_source("[(1) (2)]\n[(3)]", success_creation);
  REQUIRE(released.ok);
  REQUIRE(success_events.size() == 5U);
  release_rewrite_evaluation_result(released);
  REQUIRE(success_events.size() == 15U);
  const std::array<std::size_t, 5> success_release_order{
      {3U, 4U, 1U, 0U, 2U}};
  for (std::size_t index = 0U; index < success_release_order.size();
       ++index) {
    const ResourceLifetimeEvent &logical =
        success_events[5U + index * 2U];
    const ResourceLifetimeEvent &physical =
        success_events[6U + index * 2U];
    CHECK(logical.kind == ResourceLifetimeEventKind::logical_release);
    CHECK(physical.kind == ResourceLifetimeEventKind::physical_release);
    CHECK(logical.allocation_ordinal ==
          std::optional<std::size_t>{success_release_order[index]});
    CHECK(physical.allocation_ordinal == logical.allocation_ordinal);
  }

  std::vector<ResourceLifetimeEvent> failure_events;
  const RewriteEvaluationCreationData outer_failure_creation{
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{4U},
      ResourceLifetimeObserver{&failure_events, record_event}};
  RewriteEvaluationResult outer_failure = evaluate_rewrite_source(
      "[(1)]\n[(2) [3]]", outer_failure_creation);
  REQUIRE_FALSE(outer_failure.ok);
  CHECK(outer_failure.resources.live_evaluation_bytes == 0U);
  REQUIRE(failure_events.size() == 12U);
  const std::array<std::size_t, 4> failure_release_order{
      {3U, 2U, 0U, 1U}};
  for (std::size_t index = 0U; index < failure_release_order.size();
       ++index) {
    const ResourceLifetimeEvent &logical =
        failure_events[4U + index * 2U];
    const ResourceLifetimeEvent &physical =
        failure_events[5U + index * 2U];
    CHECK(logical.kind == ResourceLifetimeEventKind::logical_release);
    CHECK(physical.kind == ResourceLifetimeEventKind::physical_release);
    CHECK(logical.allocation_ordinal ==
          std::optional<std::size_t>{failure_release_order[index]});
    CHECK(physical.allocation_ordinal == logical.allocation_ordinal);
  }
}

TEST_CASE("TUP-050-DIRECT-PRESERVATION") {
  const RewriteEvaluationCreationData trusted_v2{
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult type =
      evaluate_rewrite_source("inc[[1 2]]", trusted_v2);
  REQUIRE_FALSE(type.ok);
  CHECK(type.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(type.diagnostic.error.argument_position == 1U);
  REQUIRE(type.diagnostic.error.type.has_value());
  REQUIRE(type.diagnostic.error.type->actual_arguments.size() == 1U);
  TypeFormattingResult actual =
      format_type(type.diagnostic.error.type->actual_arguments[0]);
  REQUIRE(actual.ok);
  CHECK(actual.formatted == "Tuple<Int, Int>");
  CHECK(type.diagnostic.primary.begin.offset == 5U);
  CHECK(type.diagnostic.primary.end.offset == 10U);

  RewriteEvaluationResult arity =
      evaluate_rewrite_source("add[[1 2]]", trusted_v2);
  REQUIRE_FALSE(arity.ok);
  CHECK(arity.diagnostic.error.kind == ErrorKind::arity_error);
  REQUIRE(arity.diagnostic.error.arity.has_value());
  CHECK(arity.diagnostic.error.arity->supplied == 1U);
  CHECK(arity.scalar_kernel_invocations == 0U);

  RewriteEvaluationResult adjacent =
      evaluate_rewrite_source("add[1 2]", trusted_v2);
  REQUIRE(adjacent.ok);
  REQUIRE(adjacent.formatted.size() == 1U);
  CHECK(adjacent.formatted[0] == "3");
  release_rewrite_evaluation_result(adjacent);

  RewriteEvaluationResult prefix =
      evaluate_rewrite_source("add [1 2]", trusted_v2);
  REQUIRE(prefix.ok);
  REQUIRE(prefix.formatted.size() == 1U);
  CHECK(prefix.formatted[0] == "3");
  CHECK(prefix.scalar_kernel_invocations == 1U);
  release_rewrite_evaluation_result(prefix);

  RewriteEvaluationResult empty_prefix =
      evaluate_rewrite_source("add []", trusted_v2);
  REQUIRE_FALSE(empty_prefix.ok);
  CHECK(empty_prefix.diagnostic.error.kind == ErrorKind::arity_error);
  REQUIRE(empty_prefix.diagnostic.error.arity.has_value());
  CHECK(empty_prefix.diagnostic.error.arity->supplied == 0U);
  CHECK(empty_prefix.scalar_kernel_invocations == 0U);

  const std::array<Value, 1> arguments{{make_int_value(4)}};
  const std::array<std::string_view, 3> parameter_sources{{
      "parameters[x Int]\n[x x]",
      "parameters[x Int]\n[inc x]",
      "parameters[x Int]\n[x inc x]",
  }};
  const std::array<std::string_view, 3> parameter_expected{{
      "[4 4]", "[5]", "[4 5]"}};
  for (std::size_t index = 0U; index < parameter_sources.size(); ++index) {
    RewriteEvaluationResult parameter = evaluate_rewrite_source_impl(
        parameter_sources[index], trusted_v2, false, arguments, {}, false,
        nullptr, nullptr, nullptr, nullptr);
    REQUIRE(parameter.ok);
    REQUIRE(parameter.formatted.size() == 1U);
    CHECK(parameter.formatted[0] == parameter_expected[index]);
    release_rewrite_evaluation_result(parameter);
  }
}

TEST_CASE("TUP-010-STATIC-SPREAD") {
  const RewriteEvaluationCreationData trusted_v2{
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{std::nullopt}};

  RewriteEvaluationResult agreement = evaluate_rewrite_source(
      "add [1 2]\nadd[1 2]\ninc [5]\ninc 5", trusted_v2);
  REQUIRE(agreement.ok);
  REQUIRE(agreement.formatted.size() == 4U);
  CHECK(agreement.formatted[0] == "3");
  CHECK(agreement.formatted[1] == "3");
  CHECK(agreement.formatted[2] == "6");
  CHECK(agreement.formatted[3] == "6");
  CHECK(agreement.scalar_kernel_invocations == 4U);
  CHECK(agreement.resources.live_evaluation_bytes == 0U);
  release_rewrite_evaluation_result(agreement);

  RewriteParseResult parsed = parse_rewrite("add [1 2]");
  REQUIRE(parsed.ok);
  REQUIRE(resolve_rewrite_primitives(parsed.program).ok);
  RewriteLoweringResult lowered = lower_rewrite_program(parsed.program);
  REQUIRE(lowered.ok);
  CHECK(rewrite_lowering_invariants_hold(parsed.program, lowered.program));
  REQUIRE(lowered.program.nodes.size() == 4U);
  const RewriteLoweringNode &spread_call = lowered.program.nodes[3U];
  CHECK(spread_call.spreads_tuple);
  CHECK(spread_call.spread_operand == 2U);
  CHECK(spread_call.argument_count == 2U);
  CHECK(spread_call.implementation ==
        PrimitiveImplementation::add_integer);

  RewriteEvaluationResult singleton =
      evaluate_rewrite_source("inc [41]", trusted_v2);
  REQUIRE(singleton.ok);
  REQUIRE(singleton.formatted.size() == 1U);
  CHECK(singleton.formatted[0] == "42");
  release_rewrite_evaluation_result(singleton);

  RewriteEvaluationResult empty =
      evaluate_rewrite_source("inc []", trusted_v2);
  REQUIRE_FALSE(empty.ok);
  CHECK(empty.diagnostic.error.kind == ErrorKind::arity_error);
  REQUIRE(empty.diagnostic.error.arity.has_value());
  CHECK(empty.diagnostic.error.arity->supplied == 0U);

  RewriteEvaluationResult nested =
      evaluate_rewrite_source("add [1 [2 3]]", trusted_v2);
  REQUIRE_FALSE(nested.ok);
  CHECK(nested.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(nested.diagnostic.error.argument_position == 2U);
  REQUIRE(nested.diagnostic.arguments.size() == 2U);
  CHECK(span_is(nested.diagnostic.arguments[0], 6U, 1U, 6U, 7U, 1U,
                7U));
  CHECK(span_is(nested.diagnostic.arguments[1], 8U, 1U, 8U, 13U, 1U,
                13U));
  CHECK(span_is(nested.diagnostic.primary, 8U, 1U, 8U, 13U, 1U,
                13U));
  CHECK(span_is(nested.diagnostic.primitive_name, 1U, 1U, 1U, 4U, 1U,
                4U));
  CHECK(span_is(nested.diagnostic.call, 1U, 1U, 1U, 14U, 1U, 14U));

  RewriteEvaluationResult computed =
      evaluate_rewrite_source("add [inc 1 true]", trusted_v2);
  REQUIRE_FALSE(computed.ok);
  CHECK(computed.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(computed.diagnostic.error.argument_position == 2U);
  REQUIRE(computed.diagnostic.arguments.size() == 2U);
  CHECK(span_is(computed.diagnostic.arguments[0], 6U, 1U, 6U, 11U, 1U,
                11U));
  CHECK(span_is(computed.diagnostic.arguments[1], 12U, 1U, 12U, 16U,
                1U, 16U));
  CHECK(span_is(computed.diagnostic.primary, 12U, 1U, 12U, 16U, 1U,
                16U));
  CHECK(computed.scalar_kernel_invocations == 0U);

  RewriteEvaluationResult dependent = evaluate_rewrite_source(
      "add [add[1 true]]", trusted_v2);
  REQUIRE_FALSE(dependent.ok);
  CHECK(dependent.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(span_is(dependent.diagnostic.primary, 12U, 1U, 12U, 16U, 1U,
                16U));

  RewriteEvaluationResult cross_root = evaluate_rewrite_source(
      "add [1 true]\nadd [1]", trusted_v2);
  REQUIRE_FALSE(cross_root.ok);
  CHECK(cross_root.diagnostic.error.kind == ErrorKind::arity_error);
  REQUIRE(cross_root.diagnostic.error.arity.has_value());
  CHECK(cross_root.diagnostic.error.arity->supplied == 1U);
  CHECK(cross_root.diagnostic.error.location.line == 2U);
  CHECK(cross_root.scalar_kernel_invocations == 0U);

  RewriteEvaluationResult mixed_cross_root = evaluate_rewrite_source(
      "add [1]\ninc[1 2]", trusted_v2);
  REQUIRE_FALSE(mixed_cross_root.ok);
  CHECK(mixed_cross_root.diagnostic.error.kind == ErrorKind::arity_error);
  REQUIRE(mixed_cross_root.diagnostic.error.arity.has_value());
  CHECK(mixed_cross_root.diagnostic.error.arity->supplied == 1U);
  CHECK(mixed_cross_root.diagnostic.error.location.line == 1U);
  CHECK(span_is(mixed_cross_root.diagnostic.call, 1U, 1U, 1U, 8U, 1U,
                8U));

  RewriteEvaluationResult dependency_cross_root = evaluate_rewrite_source(
      "add [add[1 true]]\nadd [1]", trusted_v2);
  REQUIRE_FALSE(dependency_cross_root.ok);
  CHECK(dependency_cross_root.diagnostic.error.kind ==
        ErrorKind::arity_error);
  REQUIRE(dependency_cross_root.diagnostic.error.arity.has_value());
  CHECK(dependency_cross_root.diagnostic.error.arity->supplied == 1U);
  CHECK(dependency_cross_root.diagnostic.error.location.line == 2U);

  RewriteEvaluationResult domain = evaluate_rewrite_source(
      "inc [9223372036854775807]", trusted_v2);
  REQUIRE_FALSE(domain.ok);
  CHECK(domain.diagnostic.error.kind == ErrorKind::domain_error);
  CHECK(domain.diagnostic.error.argument_position == 1U);
  CHECK(span_is(domain.diagnostic.primary, 6U, 1U, 6U, 25U, 1U, 25U));
  CHECK(span_is(domain.diagnostic.primitive_name, 1U, 1U, 1U, 4U, 1U,
                4U));
  CHECK(span_is(domain.diagnostic.call, 1U, 1U, 1U, 26U, 1U, 26U));
  CHECK(domain.resources.live_evaluation_bytes == 0U);

  RewriteEvaluationResult vectors = evaluate_rewrite_source(
      "add [iota[3] iota[3]]", trusted_v2);
  REQUIRE(vectors.ok);
  REQUIRE(vectors.formatted.size() == 1U);
  CHECK(vectors.formatted[0] == "(2 4 6)");
  CHECK(vectors.resources.live_evaluation_bytes == 24U);
  release_rewrite_evaluation_result(vectors);

  for (std::size_t ordinal = 0U; ordinal < 4U; ++ordinal) {
    const RewriteEvaluationCreationData failure{
        ExecutionProfile::trusted_local_v2,
        ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                       std::nullopt},
        AllocationFailureInjection{ordinal}};
    RewriteEvaluationResult failed =
        evaluate_rewrite_source("add [(1) (2)]", failure);
    INFO(ordinal);
    REQUIRE_FALSE(failed.ok);
    CHECK(failed.diagnostic.error.kind == ErrorKind::resource_error);
    REQUIRE(failed.diagnostic.error.resource.has_value());
    CHECK(failed.diagnostic.error.resource->reason ==
          ResourceErrorReason::allocation_unavailable);
    CHECK(failed.resources.reservation_ordinal == ordinal + 1U);
    CHECK(failed.resources.live_evaluation_bytes == 0U);
    CHECK(failed.values.empty());
    CHECK(failed.formatted.empty());
  }

  CEmissionResult emitted = emit_rewrite_c_source_impl(
      "add [1 2]",
      c_backend_configuration(EvaluationConfiguration{
          trusted_v2.profile, trusted_v2.limits,
          trusted_v2.allocation_failure}),
      nullptr, nullptr);
  REQUIRE(emitted.ok);
  CHECK(emitted.source.find(
            "&((BennuValue *)bennu_values[2].data)[0]") !=
        std::string::npos);
  CHECK(emitted.source.find(
            "&((BennuValue *)bennu_values[2].data)[1]") !=
        std::string::npos);
  const std::size_t apply_position =
      emitted.source.find("BENNU_IMPL_ADD_INT");
  const std::size_t release_position = emitted.source.find(
      "bennu_release(&bennu_resources, &bennu_values[2]);",
      apply_position);
  REQUIRE(apply_position != std::string::npos);
  REQUIRE(release_position != std::string::npos);
  CHECK(apply_position < release_position);
}

TEST_CASE("TUP-011-SPREAD-RUNTIME") {
  const RewriteEvaluationCreationData trusted_v2{
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult vectors = evaluate_rewrite_source(
      "add [iota[3] iota[3]]", trusted_v2);
  REQUIRE(vectors.ok);
  REQUIRE(vectors.formatted.size() == 1U);
  CHECK(vectors.formatted[0] == "(2 4 6)");
  CHECK(vectors.resources.live_evaluation_bytes == 24U);
  release_rewrite_evaluation_result(vectors);
}

TEST_CASE("TUP-013-PROVENANCE") {
  const RewriteEvaluationCreationData trusted_v2{
      ExecutionProfile::trusted_local_v2,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt},
      AllocationFailureInjection{std::nullopt}};
  RewriteEvaluationResult computed =
      evaluate_rewrite_source("add [inc 1 true]", trusted_v2);
  REQUIRE_FALSE(computed.ok);
  CHECK(computed.diagnostic.error.kind == ErrorKind::type_mismatch);
  CHECK(computed.diagnostic.error.argument_position == 2U);
  REQUIRE(computed.diagnostic.arguments.size() == 2U);
  CHECK(span_is(computed.diagnostic.arguments[0], 6U, 1U, 6U, 11U,
                1U, 11U));
  CHECK(span_is(computed.diagnostic.primary, 12U, 1U, 12U, 16U, 1U,
                16U));
  CHECK(span_is(computed.diagnostic.primitive_name, 1U, 1U, 1U, 4U,
                1U, 4U));
  CHECK(span_is(computed.diagnostic.call, 1U, 1U, 1U, 17U, 1U, 17U));
  REQUIRE(computed.diagnostic.has_operand);
  CHECK(span_is(computed.diagnostic.operand, 5U, 1U, 5U, 17U, 1U,
                17U));

  constexpr std::string_view runtime_source =
      "add [inc 9223372036854775806 1]";
  RewriteEvaluationResult runtime =
      evaluate_rewrite_source(runtime_source, trusted_v2);
  REQUIRE_FALSE(runtime.ok);
  CHECK(runtime.diagnostic.error.kind == ErrorKind::domain_error);
  REQUIRE(runtime.diagnostic.has_operand);
  CHECK(span_is(runtime.diagnostic.primitive_name, 1U, 1U, 1U, 4U,
                1U, 4U));
  CHECK(span_is(runtime.diagnostic.call, 1U, 1U, 1U, 32U, 1U, 32U));
  CHECK(span_is(runtime.diagnostic.operand, 5U, 1U, 5U, 32U, 1U,
                32U));
  REQUIRE(runtime.diagnostic.arguments.size() == 2U);
  CHECK(span_is(runtime.diagnostic.arguments[0], 6U, 1U, 6U, 29U,
                1U, 29U));
  CHECK(span_is(runtime.diagnostic.arguments[1], 30U, 1U, 30U, 31U,
                1U, 31U));

  ProgramResult public_runtime =
      evaluate_source(runtime_source);
  REQUIRE_FALSE(public_runtime.ok);
  const Error &public_error = public_runtime.error;
  REQUIRE(public_error.primitive_span.has_value());
  REQUIRE(public_error.call_span.has_value());
  REQUIRE(public_error.operand_span.has_value());
  REQUIRE(public_error.semantic_origins.size() == 2U);
  const auto source_span_is = [](const SourceSpan &span,
                                 std::size_t begin,
                                 std::size_t end) {
    return span.begin.offset == begin && span.begin.line == 1U &&
           span.begin.column == begin && span.end.offset == end &&
           span.end.line == 1U && span.end.column == end;
  };
  CHECK(source_span_is(*public_error.primitive_span, 1U, 4U));
  CHECK(source_span_is(*public_error.call_span, 1U, 32U));
  CHECK(source_span_is(*public_error.operand_span, 5U, 32U));
  CHECK(source_span_is(public_error.semantic_origins[0], 6U, 29U));
  CHECK(source_span_is(public_error.semantic_origins[1], 30U, 31U));

  CEmissionResult emitted = emit_rewrite_c_source_impl(
      runtime_source, c_backend_configuration(
                          trusted_local_evaluation_configuration()),
      nullptr, nullptr);
  REQUIRE(emitted.ok);
  CHECK(emitted.source.find("bennu_apply_spread") != std::string::npos);
  CHECK(emitted.source.find("failure_semantic_origins[2]") !=
        std::string::npos);
  CHECK(emitted.source.find(
            "bennu_source_location(6U, 1U, 6U), "
            "bennu_source_location(29U, 1U, 29U)") !=
        std::string::npos);
  CHECK(emitted.source.find(
            "bennu_source_location(30U, 1U, 30U), "
            "bennu_source_location(31U, 1U, 31U)") !=
        std::string::npos);
}

TEST_CASE("rewrite evaluator uses one deterministic allocation seam") {
  const RewriteParseResult parsed_literal = parse_rewrite("(1)");
  REQUIRE(parsed_literal.ok);
  REQUIRE(parsed_literal.program.nodes.size() == 1U);
  EvaluationResources malformed_literal_resources =
      make_evaluation_resources(
          ExecutionProfile::bounded_v1,
          ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
          AllocationFailureInjection{std::nullopt}, 0U, 0U, 0U);
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
  CHECK(vector_exact.resources.owner.token == 0U);

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
    CHECK(evaluated.resources.owner.token == 0U);
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
  CHECK(parsed_nested.resources.owner.token == 0U);

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
  CHECK(parsed_allocation.resources.owner.token == 0U);
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
  CHECK(diagnostic.formatting_root_position == 2U);
  CHECK(diagnostic.formatting_error == ValueFormatError::invalid_value);
  CHECK(diagnostic.formatting_invariant ==
        ValueInvariant::inactive_scalar_field);
  CHECK(span_is(diagnostic.primary, 3U, 2U, 1U, 4U, 2U, 2U));
  const Error public_error = public_error_from_diagnostic("1\n2", diagnostic);
  CHECK(public_error.kind == ErrorKind::formatting_error);
  REQUIRE(public_error.formatting.has_value());
  CHECK(public_error.formatting->reason == ValueFormatError::invalid_value);
  CHECK(public_error.formatting->root_position == 2U);
  CHECK(public_error.formatting->root_span.begin.offset == 3U);
  CHECK(public_error.formatting->root_span.end.offset == 4U);
  CHECK(public_error.formatting->invalid_value_invariant ==
        std::optional<ValueInvariant>{ValueInvariant::inactive_scalar_field});
  EvaluationResources resources =
      make_trusted_local_resources(AllocationFailureInjection{std::nullopt});
  release_rewrite_values(resources, values);
}

TEST_CASE("runner scalar text decoding agrees with typed public evaluation") {
  constexpr std::string_view source =
      "parameters[count Int ratio Double enabled Bool]\n"
      "count\nratio\nenabled\n";
  const std::array<std::string_view, 3> text{{"-5", "-0e0", "true"}};
  RunnerEvaluationResult decoded = evaluate_runner_source(source, text);

  std::array<Value, 3> typed{{make_int_value(-5), make_double_value(-0.0),
                              make_bool_value(true)}};
  ProgramResult direct = evaluate_source(source, typed);
  REQUIRE(decoded.ok);
  REQUIRE(direct.ok);
  REQUIRE(decoded.values.size() == direct.values.size());
  for (std::size_t index = 0U; index < decoded.values.size(); ++index) {
    const ValueFormattingResult decoded_format = format_value(decoded.values[index]);
    const ValueFormattingResult direct_format = format_value(direct.values[index]);
    REQUIRE(decoded_format.ok);
    REQUIRE(direct_format.ok);
    CHECK(decoded_format.formatted == direct_format.formatted);
    destroy_value(decoded.values[index]);
    destroy_value(direct.values[index]);
  }
}

TEST_CASE("runner scalar text failures preserve structured argument context") {
  constexpr std::string_view source =
      "parameters[value Int]\nvalue\n";
  const std::array<std::string_view, 1> malformed{{"9223372036854775808"}};
  RunnerEvaluationResult result = evaluate_runner_source(source, malformed);
  REQUIRE_FALSE(result.ok);
  CHECK(result.values.empty());
  CHECK(result.error.kind == ErrorKind::argument_error);
  REQUIRE(result.error.argument.has_value());
  CHECK(result.error.argument->reason == ArgumentErrorReason::out_of_range);
  CHECK(result.error.argument->required_count == 1U);
  CHECK(result.error.argument->supplied_count == 1U);
  CHECK(result.error.argument->position == 1U);
  CHECK(result.error.argument->parameter_name == std::optional<std::string>{"value"});
  CHECK(result.error.argument->expected_type ==
        std::optional<ScalarType>{ScalarType::integer});
  REQUIRE(result.error.argument->declaration_span.has_value());
  CHECK(result.error.argument->declaration_span->begin.offset == 12U);
  CHECK(result.error.argument->declaration_span->end.offset == 21U);

  constexpr std::string_view invalid_program =
      "parameters[value Int]\nvalue\nwat[1]\n";
  const std::array<std::string_view, 1> invalid_text{{"bad"}};
  RunnerEvaluationResult static_failure =
      evaluate_runner_source(invalid_program, invalid_text);
  REQUIRE_FALSE(static_failure.ok);
  CHECK(static_failure.error.kind == ErrorKind::unknown_name);
  CHECK_FALSE(static_failure.error.argument.has_value());
  CHECK(static_failure.error.location.line == 3U);
}

TEST_CASE("runner result teardown releases vector and nested tuple roots") {
  constexpr std::string_view source =
      "iota[3]\n"
      "[1 [2 true]]\n";
  RunnerEvaluationResult result = evaluate_runner_source(source, {});

  REQUIRE(result.ok);
  REQUIRE(result.values.size() == 2U);
  CHECK(result.values[0].container == ContainerKind::vector);
  CHECK(result.values[0].vector.integers != nullptr);
  CHECK(result.values[0].vector.accounting_active);
  CHECK(result.values[1].container == ContainerKind::tuple);
  CHECK(result.values[1].tuple.nodes.size == 4U);
  CHECK(result.values[1].tuple.reservations.size == 1U);
  CHECK(result.values[1].tuple.root_reservation.accounting_active);
  CHECK(result.values[1]
            .tuple.reservations.storage.get()[0]
            .accounting_active);

  CHECK(destroy_runner_evaluation_result(result));
  CHECK(result.values.empty());
  CHECK(result.formatted.empty());
  CHECK(destroy_runner_evaluation_result(result));
  CHECK(result.values.empty());
  CHECK(result.formatted.empty());
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
      evaluate_rewrite_source_impl(source, creation, true, {}, {}, false,
                                   nullptr, nullptr, nullptr, nullptr);
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
  release_evaluation_resources(evaluated.resources);
  return ValueResult{
      true, std::move(value),
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U})};
}

ProgramResult evaluate_source(std::string_view source) {
  return evaluate_source(source, std::span<const Value>{},
                         trusted_local_evaluation_configuration());
}

ProgramResult evaluate_source(
    std::string_view source,
    const EvaluationConfiguration &configuration) {
  return evaluate_source(source, std::span<const Value>{}, configuration);
}

ProgramResult evaluate_source(std::string_view source,
                              std::span<const Value> arguments) {
  return evaluate_source(source, arguments,
                         trusted_local_evaluation_configuration());
}

ProgramResult evaluate_source(
    std::string_view source, std::span<const Value> arguments,
    const EvaluationConfiguration &configuration) {
  const RewriteEvaluationCreationData creation{
      configuration.profile, configuration.limits,
      configuration.allocation_failure};
  RewriteEvaluationResult evaluated =
      evaluate_rewrite_source_impl(source, creation, false, arguments, {},
                                   false, nullptr, nullptr, nullptr, nullptr);
  if (!evaluated.ok) {
    Error error = public_error_from_diagnostic(source, evaluated.diagnostic);
    release_rewrite_evaluation_result(evaluated);
    return ProgramResult{false, {}, std::move(error)};
  }
  std::vector<Value> values = std::move(evaluated.values);
  evaluated.values.clear();
  evaluated.formatted.clear();
  release_evaluation_resources(evaluated.resources);
  return ProgramResult{
      true, std::move(values),
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U})};
}

RunnerEvaluationResult evaluate_runner_source(
    std::string_view source, std::span<const std::string_view> arguments) {
  const EvaluationConfiguration configuration =
      trusted_local_evaluation_configuration();
  const RewriteEvaluationCreationData creation{
      configuration.profile, configuration.limits,
      configuration.allocation_failure};
  RewriteEvaluationResult evaluated = evaluate_rewrite_source_impl(
      source, creation, false, {}, arguments, true, nullptr, nullptr, nullptr,
      nullptr);
  if (!evaluated.ok) {
    Error error = public_error_from_diagnostic(source, evaluated.diagnostic);
    release_rewrite_evaluation_result(evaluated);
    return RunnerEvaluationResult{false, {}, {}, std::move(error)};
  }
  std::vector<Value> values = std::move(evaluated.values);
  std::vector<std::string> formatted = std::move(evaluated.formatted);
  evaluated.values.clear();
  evaluated.formatted.clear();
  release_evaluation_resources(evaluated.resources);
  return RunnerEvaluationResult{
      true, std::move(values), std::move(formatted),
      make_error(ErrorKind::none, SourceLocation{1U, 1U, 1U})};
}

bool destroy_runner_evaluation_result(RunnerEvaluationResult &result) {
  bool destroyed = true;
  for (Value &value : result.values) {
    if (!destroy_value(value).ok) {
      destroyed = false;
    }
  }
  if (destroyed) {
    result.values.clear();
  }
  result.formatted.clear();
  return destroyed;
}

CEmissionResult emit_c_source(std::string_view source) {
  return emit_rewrite_c_source_impl(source,
                                    trusted_local_c_configuration(), nullptr,
                                    nullptr);
}

CEmissionResult emit_c_source(
    std::string_view source,
    const EvaluationConfiguration &configuration) {
  return emit_rewrite_c_source_impl(
      source, c_backend_configuration(configuration), nullptr, nullptr);
}

} // namespace bennu
