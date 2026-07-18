#include "bennu/evaluator.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
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

void test_integer_expression_returns_scalar_value() {
  const bennu::ValueResult result = bennu::evaluate_expression("5");

  check(result.ok, "integer expression succeeds");
  if (result.ok) {
    check(result.value.kind == bennu::ValueKind::integer,
          "integer expression returns scalar kind");
    check(result.value.integer == std::int64_t{5},
          "integer expression preserves its value");
  }
}

void test_signed_integer_boundaries_are_preserved() {
  const bennu::ValueResult minimum =
      bennu::evaluate_expression("-9223372036854775808");
  const bennu::ValueResult maximum =
      bennu::evaluate_expression("9223372036854775807");

  check(minimum.ok, "minimum signed 64-bit integer succeeds");
  check(maximum.ok, "maximum signed 64-bit integer succeeds");
  if (minimum.ok) {
    check(minimum.value.integer == INT64_MIN,
          "minimum signed 64-bit integer is exact");
  }
  if (maximum.ok) {
    check(maximum.value.integer == INT64_MAX,
          "maximum signed 64-bit integer is exact");
  }
}

void test_inc_increments_an_integer() {
  const bennu::ValueResult result = bennu::evaluate_expression("inc 5");

  check(result.ok, "inc integer succeeds");
  if (result.ok) {
    check(result.value.kind == bennu::ValueKind::integer,
          "inc returns a scalar kind");
    check(result.value.integer == std::int64_t{6},
          "inc adds one to its argument");
  }
}

void test_ioata_builds_one_through_positive_argument() {
  const bennu::ValueResult result = bennu::evaluate_expression("ioata 5");

  check(result.ok, "ioata positive integer succeeds");
  if (result.ok) {
    check(result.value.kind == bennu::ValueKind::array,
          "ioata returns an array kind");
    check(result.value.elements ==
              std::vector<std::int64_t>{1, 2, 3, 4, 5},
          "ioata returns one through its argument");
  }
}

void test_value_formatting_is_exact() {
  const bennu::ValueResult scalar = bennu::evaluate_expression("inc 5");
  const bennu::ValueResult array = bennu::evaluate_expression("ioata 5");
  const bennu::ValueResult zero = bennu::evaluate_expression("ioata 0");
  const bennu::ValueResult negative = bennu::evaluate_expression("ioata -5");

  check(scalar.ok && bennu::format_value(scalar.value) == "6",
        "scalar formatting is exact");
  check(array.ok && bennu::format_value(array.value) == "(1 2 3 4 5)",
        "array formatting is exact");
  check(zero.ok && bennu::format_value(zero.value) == "()",
        "ioata zero formats as an empty array");
  check(negative.ok && bennu::format_value(negative.value) == "()",
        "ioata negative formats as an empty array");
}

void test_program_returns_values_in_source_order() {
  const bennu::ProgramResult result =
      bennu::evaluate_source("ioata 5\ninc 5");

  check(result.ok, "two-expression program succeeds");
  if (result.ok) {
    check(result.values.size() == 2, "program returns two values");
    if (result.values.size() == 2) {
      check(bennu::format_value(result.values[0]) == "(1 2 3 4 5)",
            "first result stays first");
      check(bennu::format_value(result.values[1]) == "6",
            "second result stays second");
    }
  }
}

void test_tokenizer_preserves_kinds_and_source_locations() {
  const bennu::TokenizeResult result =
      bennu::tokenize(" \tioata inc -5\r\n");

  check(result.ok, "valid source tokenizes");
  if (result.ok) {
    check(result.tokens.size() == 5,
          "tokenizer emits expressions, newline, and end");
    if (result.tokens.size() == 5) {
      check(result.tokens[0].kind == bennu::TokenKind::ioata,
            "first token is ioata");
      check(result.tokens[0].location.line == 1 &&
                result.tokens[0].location.column == 3,
            "ioata location includes leading horizontal whitespace");
      check(result.tokens[1].kind == bennu::TokenKind::inc,
            "second token is inc");
      check(result.tokens[2].kind == bennu::TokenKind::integer,
            "third token is integer");
      check(result.tokens[3].kind == bennu::TokenKind::newline &&
                result.tokens[3].length == 2,
            "CRLF is one newline token");
      check(result.tokens[4].kind == bennu::TokenKind::end &&
                result.tokens[4].location.line == 2 &&
                result.tokens[4].location.column == 1,
            "end token follows CRLF");
    }
  }
}

void test_parser_builds_flat_postorder_nodes_for_nested_calls() {
  const std::string_view source = "ioata inc 5";
  const bennu::TokenizeResult tokenized = bennu::tokenize(source);
  check(tokenized.ok, "nested source tokenizes before parsing");
  if (!tokenized.ok) {
    return;
  }

  const bennu::ParseResult parsed = bennu::parse_program(source, tokenized.tokens);
  check(parsed.ok, "nested source parses");
  if (parsed.ok) {
    check(parsed.program.nodes.size() == 3,
          "nested expression produces three flat nodes");
    check(parsed.program.roots.size() == 1 && parsed.program.roots[0] == 2,
          "program root refers to final postorder node");
    if (parsed.program.nodes.size() == 3) {
      check(parsed.program.nodes[0].kind == bennu::ExpressionKind::integer,
            "integer argument is first postorder node");
      check(parsed.program.nodes[1].kind == bennu::ExpressionKind::inc &&
                parsed.program.nodes[1].argument == 0,
            "inc refers to integer node");
      check(parsed.program.nodes[2].kind == bennu::ExpressionKind::ioata &&
                parsed.program.nodes[2].argument == 1,
            "ioata refers to inc node");
    }
  }
}

void test_parser_rejects_overflowing_public_token_span() {
  const std::string_view source = "5";
  const bennu::SourceLocation invalid_location{
      std::numeric_limits<std::size_t>::max(), 7, 9};
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::integer, invalid_location, 2, true},
      {bennu::TokenKind::end, {source.size(), 1, 2}, 0, false},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects an overflowing public token span");
  check(parsed.error.location.offset == invalid_location.offset &&
            parsed.error.location.line == invalid_location.line &&
            parsed.error.location.column == invalid_location.column,
        "invalid token span error preserves the supplied location");
}

void test_parser_rejects_public_token_stream_without_end_token() {
  const std::string_view source = "5";
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::integer, {0, 1, 1}, 1, true},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects a public token stream without an end token");
  check(parsed.error.location.offset == source.size() &&
            parsed.error.location.line == 1 &&
            parsed.error.location.column == 2,
        "missing end token error identifies the end of source");
}

void test_parser_rejects_empty_public_token_stream_at_source_end() {
  const std::string_view source;
  const std::vector<bennu::Token> tokens;

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects an empty public token stream");
  check(parsed.error.location.offset == 0 && parsed.error.location.line == 1 &&
            parsed.error.location.column == 1,
        "empty token-stream error identifies the empty source end");
}

void test_parser_locates_missing_end_token_at_multiline_source_end() {
  const std::string_view source = "5\r\ninc 6";
  bennu::TokenizeResult tokenized = bennu::tokenize(source);
  check(tokenized.ok, "multiline source tokenizes before removing its end token");
  if (!tokenized.ok) {
    return;
  }
  tokenized.tokens.pop_back();

  const bennu::ParseResult parsed =
      bennu::parse_program(source, tokenized.tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects a multiline token stream without an end token");
  check(parsed.error.location.offset == source.size() &&
            parsed.error.location.line == 2 &&
            parsed.error.location.column == 6,
        "missing end-token error identifies the exact multiline source end");
}

void test_parser_rejects_nonzero_length_public_end_token() {
  const std::string_view source = "5";
  const bennu::SourceLocation invalid_location{0, 1, 1};
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::end, invalid_location, 1, true},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects a nonzero-length public end token");
  check(parsed.error.location.offset == invalid_location.offset &&
            parsed.error.location.line == invalid_location.line &&
            parsed.error.location.column == invalid_location.column,
        "invalid end-token length error preserves the supplied location");
}

void test_parser_rejects_public_end_token_before_source_end() {
  const std::string_view source = "5 6";
  const bennu::SourceLocation invalid_location{1, 1, 2};
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::integer, {0, 1, 1}, 1, true},
      {bennu::TokenKind::end, invalid_location, 0, false},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects an end token before the source ends");
  check(parsed.error.location.offset == invalid_location.offset &&
            parsed.error.location.line == invalid_location.line &&
            parsed.error.location.column == invalid_location.column,
        "early end-token error preserves the supplied location");
}

void test_parser_rejects_tokens_after_public_end_token() {
  const std::string_view source = "5 6";
  const bennu::SourceLocation trailing_location{2, 1, 3};
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::integer, {0, 1, 1}, 1, true},
      {bennu::TokenKind::end, {1, 1, 2}, 0, false},
      {bennu::TokenKind::integer, trailing_location, 1, true},
      {bennu::TokenKind::end, {source.size(), 1, 4}, 0, false},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects tokens after a public end token");
  check(parsed.error.location.offset == trailing_location.offset &&
            parsed.error.location.line == trailing_location.line &&
            parsed.error.location.column == trailing_location.column,
        "token after end error identifies the first ignored token");
}

void test_parser_rejects_unknown_public_token_kind() {
  const std::string_view source = "5";
  const bennu::SourceLocation invalid_location{0, 1, 1};
  const std::vector<bennu::Token> tokens{
      {static_cast<bennu::TokenKind>(99), invalid_location, 1, true},
      {bennu::TokenKind::end, {source.size(), 1, 2}, 0, false},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects an unknown public token kind");
  check(parsed.error.location.offset == invalid_location.offset &&
            parsed.error.location.line == invalid_location.line &&
            parsed.error.location.column == invalid_location.column,
        "unknown token kind error preserves the token location");
}

void test_parser_rejects_omitted_nonwhitespace_source() {
  const std::string_view source = "5 extra";
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::integer, {0, 1, 1}, 1, true},
      {bennu::TokenKind::end, {source.size(), 1, 8}, 0, false},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects omitted non-whitespace source");
}

void test_parser_rejects_overlapping_public_token_spans() {
  const std::string_view source = "555";
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::integer, {0, 1, 1}, 2, true},
      {bennu::TokenKind::integer, {1, 1, 2}, 2, false},
      {bennu::TokenKind::end, {source.size(), 1, 4}, 0, false},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects overlapping public token spans");
}

void test_parser_rejects_out_of_order_public_token_spans() {
  const std::string_view source = "5\n6";
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::integer, {2, 2, 1}, 1, true},
      {bennu::TokenKind::newline, {1, 1, 2}, 1, false},
      {bennu::TokenKind::integer, {0, 1, 1}, 1, true},
      {bennu::TokenKind::end, {source.size(), 2, 2}, 0, false},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects out-of-order public token spans");
}

void test_parser_rejects_public_token_kind_lexeme_mismatch() {
  const std::string_view source = "xxxxx 5";
  const std::vector<bennu::Token> tokens{
      {bennu::TokenKind::inc, {0, 1, 1}, 5, true},
      {bennu::TokenKind::integer, {6, 1, 7}, 1, true},
      {bennu::TokenKind::end, {source.size(), 1, 8}, 0, false},
  };

  const bennu::ParseResult parsed = bennu::parse_program(source, tokens);

  check(!parsed.ok && parsed.error.kind == bennu::ErrorKind::invalid_program,
        "parser rejects a public token kind that does not match its lexeme");
}

void test_flat_program_evaluator_handles_nested_prefix_calls() {
  const std::string_view source = "ioata inc 5";
  const bennu::TokenizeResult tokenized = bennu::tokenize(source);
  check(tokenized.ok, "nested evaluator source tokenizes");
  if (!tokenized.ok) {
    return;
  }
  const bennu::ParseResult parsed = bennu::parse_program(source, tokenized.tokens);
  check(parsed.ok, "nested evaluator source parses");
  if (!parsed.ok) {
    return;
  }

  const bennu::ProgramResult evaluated = bennu::evaluate_program(parsed.program);
  check(evaluated.ok, "nested flat program evaluates");
  if (evaluated.ok) {
    check(evaluated.values.size() == 1 &&
              bennu::format_value(evaluated.values[0]) == "(1 2 3 4 5 6)",
          "ioata consumes the result of nested inc");
  }
}

void test_program_evaluator_rejects_unknown_public_expression_kind() {
  const bennu::SourceLocation invalid_location{23, 4, 6};
  const bennu::Program program{
      {
          {bennu::ExpressionKind::integer, {0, 1, 1}, 3, 0},
          {static_cast<bennu::ExpressionKind>(99), invalid_location, 0, 0},
      },
      {1},
  };

  const bennu::ProgramResult evaluated = bennu::evaluate_program(program);

  check(!evaluated.ok &&
            evaluated.error.kind == bennu::ErrorKind::invalid_program,
        "program evaluator rejects an unknown public expression kind");
  check(evaluated.error.location.offset == invalid_location.offset &&
            evaluated.error.location.line == invalid_location.line &&
            evaluated.error.location.column == invalid_location.column,
        "unknown expression kind error preserves the node location");
}

void test_inc_rejects_overflow_and_array_arguments_at_the_call_site() {
  const bennu::SourceLocation location{17, 3, 4};
  const bennu::Value maximum{bennu::ValueKind::integer, INT64_MAX, {}};
  const bennu::Value array{bennu::ValueKind::array, 0, {1, 2, 3}};

  const bennu::ValueResult overflow = bennu::apply_inc(maximum, location);
  const bennu::ValueResult wrong_type = bennu::apply_inc(array, location);

  check(!overflow.ok && overflow.error.kind == bennu::ErrorKind::integer_overflow,
        "inc reports signed 64-bit overflow");
  check(!wrong_type.ok && wrong_type.error.kind == bennu::ErrorKind::type_mismatch,
        "inc rejects arrays instead of lifting");
  check(overflow.error.location.offset == 17 &&
            overflow.error.location.line == 3 &&
            overflow.error.location.column == 4,
        "inc overflow is located at the call site");
  check(wrong_type.error.location.offset == 17 &&
            wrong_type.error.location.line == 3 &&
            wrong_type.error.location.column == 4,
        "inc type error is located at the call site");
}

void test_ioata_rejects_over_limit_before_allocating() {
  const bennu::SourceLocation location{6, 1, 7};
  const bennu::Value request{bennu::ValueKind::integer,
                             bennu::ioata_element_limit + 1, {}};
  const bennu::Value boundary{bennu::ValueKind::integer,
                              bennu::ioata_element_limit, {}};
  const bennu::Value array{bennu::ValueKind::array, 0, {1}};

  const bennu::ValueResult result = bennu::apply_ioata(request, location);
  const bennu::ValueResult allowed = bennu::apply_ioata(boundary, location);
  const bennu::ValueResult wrong_type = bennu::apply_ioata(array, location);

  check(!result.ok &&
            result.error.kind == bennu::ErrorKind::allocation_limit_exceeded,
        "ioata reports its deterministic allocation limit");
  check(result.value.elements.empty(),
        "over-limit ioata does not produce an allocation-backed value");
  check(result.error.location.offset == 6 && result.error.location.line == 1 &&
            result.error.location.column == 7,
        "ioata allocation error is located at the call site");
  check(allowed.ok &&
            allowed.value.elements.size() ==
                static_cast<std::size_t>(bennu::ioata_element_limit) &&
            allowed.value.elements.back() == bennu::ioata_element_limit,
        "ioata accepts the documented allocation boundary");
  check(!wrong_type.ok &&
            wrong_type.error.kind == bennu::ErrorKind::type_mismatch,
        "ioata rejects an array argument");
}

void test_program_caps_cumulative_ioata_elements() {
  const bennu::ProgramResult result =
      bennu::evaluate_source("ioata 600000\nioata 600000");

  check(!result.ok &&
            result.error.kind == bennu::ErrorKind::allocation_limit_exceeded,
        "program rejects cumulative ioata elements over the safe limit");
  check(result.error.location.line == 2 && result.error.location.column == 1,
        "cumulative allocation error identifies the request crossing the limit");
}

void test_required_invalid_inputs_return_located_structured_errors() {
  struct ErrorCase {
    std::string_view source;
    bennu::ErrorKind kind;
    std::size_t offset;
    std::size_t line;
    std::size_t column;
  };
  const std::array<ErrorCase, 11> cases{{
      {"\n\twat", bennu::ErrorKind::unknown_name, 2, 2, 2},
      {"ioata @", bennu::ErrorKind::illegal_character, 6, 1, 7},
      {"inc", bennu::ErrorKind::missing_argument, 0, 1, 1},
      {"5 extra", bennu::ErrorKind::trailing_token, 2, 1, 3},
      {"12x", bennu::ErrorKind::malformed_integer, 0, 1, 1},
      {"-9223372036854775809", bennu::ErrorKind::integer_out_of_range, 0, 1, 1},
      {"9223372036854775808", bennu::ErrorKind::integer_out_of_range, 0, 1, 1},
      {"inc 9223372036854775807", bennu::ErrorKind::integer_overflow, 0, 1, 1},
      {"\n  inc ioata 3", bennu::ErrorKind::type_mismatch, 3, 2, 3},
      {"ioata 1000001", bennu::ErrorKind::allocation_limit_exceeded, 0, 1, 1},
      {"inc-5", bennu::ErrorKind::expected_whitespace, 3, 1, 4},
  }};

  for (const ErrorCase &error_case : cases) {
    const bennu::ValueResult result =
        bennu::evaluate_expression(error_case.source);
    if (result.ok || result.error.kind != error_case.kind ||
        result.error.location.offset != error_case.offset ||
        result.error.location.line != error_case.line ||
        result.error.location.column != error_case.column ||
        result.error.message.empty()) {
      std::cerr << "FAIL: structured error mismatch for [" << error_case.source
                << "]\n";
      ++failures;
    }
  }
}

void test_program_accepts_horizontal_space_blank_lines_crlf_and_no_final_newline() {
  const bennu::ProgramResult result =
      bennu::evaluate_source(" \tioata 2 \r\n\r\n\tinc 5\t\n\n-1");
  const bennu::ProgramResult empty = bennu::evaluate_source(" \t\r\n\n");

  check(result.ok, "mixed accepted whitespace and newline forms succeed");
  if (result.ok) {
    check(result.values.size() == 3, "blank lines do not create values");
    if (result.values.size() == 3) {
      check(bennu::format_value(result.values[0]) == "(1 2)",
            "CRLF line evaluates first");
      check(bennu::format_value(result.values[1]) == "6",
            "LF line evaluates second");
      check(bennu::format_value(result.values[2]) == "-1",
            "line without final newline evaluates last");
    }
  }
  check(empty.ok && empty.values.empty(),
        "zero-expression whitespace-only program succeeds");
}

void test_one_expression_api_locates_empty_input_at_end() {
  const bennu::ValueResult result = bennu::evaluate_expression("\r\n\t");

  check(!result.ok && result.error.kind == bennu::ErrorKind::empty_expression,
        "one-expression API rejects an empty program");
  check(result.error.location.offset == 3 && result.error.location.line == 2 &&
            result.error.location.column == 2,
        "empty-expression location is the actual end of source");
}

} // namespace

int main() {
  test_integer_expression_returns_scalar_value();
  test_signed_integer_boundaries_are_preserved();
  test_inc_increments_an_integer();
  test_ioata_builds_one_through_positive_argument();
  test_value_formatting_is_exact();
  test_program_returns_values_in_source_order();
  test_tokenizer_preserves_kinds_and_source_locations();
  test_parser_builds_flat_postorder_nodes_for_nested_calls();
  test_parser_rejects_overflowing_public_token_span();
  test_parser_rejects_public_token_stream_without_end_token();
  test_parser_rejects_empty_public_token_stream_at_source_end();
  test_parser_locates_missing_end_token_at_multiline_source_end();
  test_parser_rejects_nonzero_length_public_end_token();
  test_parser_rejects_public_end_token_before_source_end();
  test_parser_rejects_tokens_after_public_end_token();
  test_parser_rejects_unknown_public_token_kind();
  test_parser_rejects_omitted_nonwhitespace_source();
  test_parser_rejects_overlapping_public_token_spans();
  test_parser_rejects_out_of_order_public_token_spans();
  test_parser_rejects_public_token_kind_lexeme_mismatch();
  test_flat_program_evaluator_handles_nested_prefix_calls();
  test_program_evaluator_rejects_unknown_public_expression_kind();
  test_inc_rejects_overflow_and_array_arguments_at_the_call_site();
  test_ioata_rejects_over_limit_before_allocating();
  test_program_caps_cumulative_ioata_elements();
  test_required_invalid_inputs_return_located_structured_errors();
  test_program_accepts_horizontal_space_blank_lines_crlf_and_no_final_newline();
  test_one_expression_api_locates_empty_input_at_end();
  return failures == 0 ? 0 : 1;
}
