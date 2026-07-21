#include "bennu/c_emitter.hpp"
#include "bennu/native_builder.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {

bool write_file(const char *path, std::string_view bytes) {
  std::FILE *file = std::fopen(path, "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written =
      std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size();
  return std::fclose(file) == 0 && written;
}

bool read_file(const char *path, std::string &bytes) {
  std::FILE *file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  char buffer[4096];
  while (true) {
    const std::size_t count = std::fread(buffer, 1U, sizeof(buffer), file);
    bytes.append(buffer, count);
    if (count != sizeof(buffer)) {
      const bool complete = std::feof(file) != 0 && std::ferror(file) == 0;
      return std::fclose(file) == 0 && complete;
    }
  }
}

void normalize_newlines(std::string &bytes) {
  std::size_t write = 0U;
  for (std::size_t read = 0U; read < bytes.size(); ++read) {
    if (bytes[read] == '\r' && read + 1U < bytes.size() &&
        bytes[read + 1U] == '\n') {
      continue;
    }
    bytes[write] = bytes[read];
    ++write;
  }
  bytes.resize(write);
}

void destroy_program(bennu::ProgramResult &result) {
  for (bennu::Value &value : result.values) {
    bennu::destroy_value(value);
  }
  result.values.clear();
}

bool is_resource_error(const bennu::Error &error,
                       bennu::ResourceErrorReason reason) {
  return error.kind == bennu::ErrorKind::resource_error &&
         error.resource.has_value() && error.resource->reason == reason;
}

bool emit_and_build(std::string_view source,
                    const bennu::EvaluationConfiguration &configuration,
                    const char *c_path, const char *native_path,
                    const char *compiler) {
  const bennu::CEmissionResult emitted =
      bennu::emit_c_source(source, configuration);
  if (!emitted.ok || emitted.source.empty() ||
      !write_file(c_path, emitted.source)) {
    return false;
  }
  const bennu::NativeBuildResult built = bennu::build_native(
      bennu::NativeBuildRequest{emitted.source, native_path, compiler, ""});
  return built.ok;
}

std::string refusal_evidence(const bennu::Error &error) {
  if (!error.resource.has_value() || !error.primitive.has_value() ||
      !error.resource->limit_kind.has_value() ||
      !error.resource->configured_limit.has_value() ||
      !error.resource->usage_before.has_value() ||
      !error.resource->refused_charge.has_value()) {
    return {};
  }
  const std::string_view reason =
      error.resource->reason == bennu::ResourceErrorReason::profile_limit
          ? "profile_limit"
          : "unexpected";
  const std::string_view limit =
      *error.resource->limit_kind == bennu::ResourceLimitKind::max_work_units
          ? "max_work_units"
          : "unexpected";
  return "ResourceError: reason=" + std::string(reason) +
         " profile=" + error.resource->profile +
         " limit=" + std::string(limit) +
         " configured=" +
         std::to_string(*error.resource->configured_limit) +
         " usage-before=" + std::to_string(*error.resource->usage_before) +
         " refused-charge=" +
         std::to_string(*error.resource->refused_charge) +
         " admission=" + error.primitive->name +
         " source=" + std::to_string(error.location.offset) + ":" +
         std::to_string(error.location.line) + ":" +
         std::to_string(error.location.column) + "\n";
}

} // namespace

int main(int argument_count, char **arguments) {
  if (argument_count != 15) {
    return 2;
  }

  std::string corpus_source;
  std::string corpus_expected;
  if (!read_file(arguments[2], corpus_source) ||
      !read_file(arguments[3], corpus_expected)) {
    return 3;
  }
  normalize_newlines(corpus_expected);
  bennu::ProgramResult corpus = bennu::evaluate_source(corpus_source);
  if (!corpus.ok) {
    destroy_program(corpus);
    return 4;
  }
  std::string corpus_actual;
  for (bennu::Value &value : corpus.values) {
    const bennu::ValueFormattingResult formatted = bennu::format_value(value);
    if (!formatted.ok) {
      destroy_program(corpus);
      return 5;
    }
    corpus_actual += formatted.formatted;
    corpus_actual.push_back('\n');
  }
  destroy_program(corpus);
  if (corpus_actual != corpus_expected) {
    return 6;
  }

  struct ErrorCase {
    std::string_view source;
    bennu::ErrorKind kind;
  };
  const ErrorCase error_cases[] = {
      {"é", bennu::ErrorKind::invalid_byte},
      {"12x", bennu::ErrorKind::malformed_literal},
      {"9223372036854775808", bennu::ErrorKind::literal_range_error},
      {"inc ", bennu::ErrorKind::syntax_error},
      {"wat[1]", bennu::ErrorKind::unknown_name},
      {"add[1]", bennu::ErrorKind::arity_error},
      {"add[1 true]", bennu::ErrorKind::type_mismatch},
      {"add[(1 2) (3)]", bennu::ErrorKind::shape_mismatch},
      {"inc 9223372036854775807", bennu::ErrorKind::domain_error},
      {"iota[2305843009213693952]", bennu::ErrorKind::resource_error},
  };
  for (const ErrorCase &error_case : error_cases) {
    bennu::ValueResult failure = bennu::evaluate_expression(error_case.source);
    if (failure.ok || failure.error.kind != error_case.kind) {
      if (failure.ok) {
        bennu::destroy_value(failure.value);
      }
      return 7;
    }
  }
  bennu::ProgramResult late_error =
      bennu::evaluate_source("inc 5\nwat[1]\n");
  if (late_error.ok || !late_error.values.empty() ||
      late_error.error.kind != bennu::ErrorKind::unknown_name ||
      late_error.error.location.line != 2U) {
    destroy_program(late_error);
    return 8;
  }

  constexpr std::string_view profile_source =
      "inc[(1)]\n"
      "inc[(1)]\n";
  const bennu::EvaluationConfiguration exact_profile{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::size_t{24U},
                            std::size_t{2U}},
      bennu::AllocationFailureInjection{std::nullopt}};
  for (std::size_t invocation = 0U; invocation < 2U; ++invocation) {
    bennu::ProgramResult exact =
        bennu::evaluate_source(profile_source, exact_profile);
    if (!exact.ok || exact.values.size() != 2U) {
      destroy_program(exact);
      return 20;
    }
    destroy_program(exact);
  }

  const bennu::EvaluationConfiguration one_past_profile{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::size_t{24U},
                            std::size_t{1U}},
      bennu::AllocationFailureInjection{std::nullopt}};
  bennu::ProgramResult refused =
      bennu::evaluate_source(profile_source, one_past_profile);
  if (refused.ok || !refused.values.empty() ||
      !is_resource_error(refused.error,
                         bennu::ResourceErrorReason::profile_limit) ||
      refused.error.resource->limit_kind !=
          bennu::ResourceLimitKind::max_work_units ||
      refused.error.resource->configured_limit != std::size_t{1U} ||
      refused.error.resource->usage_before != std::size_t{1U} ||
      refused.error.resource->refused_charge != std::size_t{1U} ||
      refused.error.resource->profile != "bounded-v1" ||
      !refused.error.primitive.has_value() ||
      refused.error.primitive->name != "inc" ||
      refused.error.location.offset != std::size_t{10U} ||
      refused.error.location.line != std::size_t{2U} ||
      refused.error.location.column != std::size_t{1U}) {
    destroy_program(refused);
    return 21;
  }
  const std::string expected_refusal = refusal_evidence(refused.error);
  if (expected_refusal.empty() ||
      !write_file(arguments[14], expected_refusal)) {
    return 22;
  }

  const bennu::EvaluationConfiguration fail_first{
      bennu::ExecutionProfile::trusted_local_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      bennu::AllocationFailureInjection{std::size_t{0U}}};
  for (std::size_t invocation = 0U; invocation < 2U; ++invocation) {
    bennu::ValueResult iota =
        bennu::evaluate_expression("iota[2]", fail_first);
    if (iota.ok ||
        !is_resource_error(iota.error,
                           bennu::ResourceErrorReason::allocation_unavailable) ||
        iota.error.resource->requested_elements != std::size_t{2U} ||
        iota.error.resource->requested_bytes != std::size_t{16U}) {
      if (iota.ok) {
        bennu::destroy_value(iota.value);
      }
      return 23;
    }
  }

  const bennu::EvaluationConfiguration fail_second{
      bennu::ExecutionProfile::trusted_local_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      bennu::AllocationFailureInjection{std::size_t{1U}}};
  bennu::ValueResult lifted =
      bennu::evaluate_expression("inc[(1 2)]", fail_second);
  if (lifted.ok ||
      !is_resource_error(lifted.error,
                         bennu::ResourceErrorReason::allocation_unavailable)) {
    if (lifted.ok) {
      bennu::destroy_value(lifted.value);
    }
    return 24;
  }
  bennu::ProgramResult late =
      bennu::evaluate_source("iota[2]\niota[2]\n", fail_second);
  if (late.ok || !late.values.empty() ||
      !is_resource_error(late.error,
                         bennu::ResourceErrorReason::allocation_unavailable)) {
    destroy_program(late);
    return 25;
  }

  bennu::ValueResult overflow = bennu::evaluate_expression(
      "iota[2305843009213693952]", exact_profile);
  if (overflow.ok ||
      !is_resource_error(overflow.error,
                         bennu::ResourceErrorReason::size_overflow)) {
    if (overflow.ok) {
      bennu::destroy_value(overflow.value);
    }
    return 26;
  }

  if (!emit_and_build(profile_source, exact_profile, arguments[4], arguments[5],
                      arguments[1]) ||
      !emit_and_build(profile_source, one_past_profile, arguments[12],
                      arguments[13], arguments[1]) ||
      !emit_and_build("iota[2]\n", fail_first, arguments[6], arguments[7],
                      arguments[1]) ||
      !emit_and_build("inc[(1 2)]\n", fail_second, arguments[8], arguments[9],
                      arguments[1]) ||
      !emit_and_build("iota[2]\niota[2]\n", fail_second, arguments[10],
                      arguments[11], arguments[1])) {
    return 27;
  }
  return 0;
}
