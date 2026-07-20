// Issue #33 benchmark-only translation unit. This deliberately includes the
// accepted internal evaluator and is never part of bennu or its default build.
#include "../../src/rewrite.cpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace bennu {
namespace issue33 {

constexpr std::size_t hybrid_vector_element_threshold = 1000U;

enum class ExpectedOutcome {
  success,
  invalid_source,
  resource_refusal,
};

enum class WorkloadForm {
  scalar_or_empty,
  arbitrary_lifted_int_vector,
  nested_iota_lifting,
};

struct Workload {
  std::string_view identifier;
  std::string_view source;
  std::size_t size;
  ExpectedOutcome expected;
  WorkloadForm form;
};

struct EvaluatedWorkload {
  bool ok;
  RewriteEvaluationResult evaluation;
  std::string canonical_output;
  std::string error;
};

struct EmissionResult {
  bool ok;
  std::string source;
  std::string error;
};

constexpr Workload workloads[] = {
    {"scalar_bool", "true", 1U, ExpectedOutcome::success,
     WorkloadForm::scalar_or_empty},
    {"scalar_int", "-9223372036854775808", 1U, ExpectedOutcome::success,
     WorkloadForm::scalar_or_empty},
    {"scalar_double", "-0.0", 1U, ExpectedOutcome::success,
     WorkloadForm::scalar_or_empty},
    {"empty_bool", "Bool()", 0U, ExpectedOutcome::success,
     WorkloadForm::scalar_or_empty},
    {"empty_int", "Int()", 0U, ExpectedOutcome::success,
     WorkloadForm::scalar_or_empty},
    {"empty_double", "Double()", 0U, ExpectedOutcome::success,
     WorkloadForm::scalar_or_empty},
    {"arbitrary_lifted", "inc[(7 -3 11 0)]", 4U,
     ExpectedOutcome::success, WorkloadForm::arbitrary_lifted_int_vector},
    {"nested_iota_1", "inc[inc[iota[1]]]", 1U,
     ExpectedOutcome::success, WorkloadForm::nested_iota_lifting},
    {"nested_iota_1000", "inc[inc[iota[1000]]]", 1000U,
     ExpectedOutcome::success, WorkloadForm::nested_iota_lifting},
    {"nested_iota_100000", "inc[inc[iota[100000]]]", 100000U,
     ExpectedOutcome::success, WorkloadForm::nested_iota_lifting},
    {"invalid_source", "add[1, 2]", 0U, ExpectedOutcome::invalid_source,
     WorkloadForm::scalar_or_empty},
    {"resource_refusal", "inc[iota[1000]]", 1000U,
     ExpectedOutcome::resource_refusal, WorkloadForm::nested_iota_lifting},
};

const Workload *find_workload(std::string_view identifier) {
  for (const Workload &workload : workloads) {
    if (workload.identifier == identifier) {
      return &workload;
    }
  }
  return nullptr;
}

std::string_view expected_outcome_name(ExpectedOutcome outcome) {
  if (outcome == ExpectedOutcome::success) {
    return "success";
  }
  if (outcome == ExpectedOutcome::invalid_source) {
    return "invalid_source";
  }
  return "resource_refusal";
}

RewriteEvaluationCreationData creation_data(const Workload &workload) {
  if (workload.expected == ExpectedOutcome::resource_refusal) {
    return RewriteEvaluationCreationData{
        ExecutionProfile::bounded_v1,
        ResourceLimits{64U, std::nullopt, std::nullopt},
        AllocationFailureInjection{std::nullopt}};
  }
  return RewriteEvaluationCreationData{
      ExecutionProfile::trusted_local_v1,
      ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      AllocationFailureInjection{std::nullopt}};
}

std::string joined_formatted_output(
    const std::vector<std::string> &formatted) {
  std::string output;
  for (const std::string &root : formatted) {
    output.append(root);
    output.push_back('\n');
  }
  return output;
}

// ISSUE33_COMPLEXITY_BEGIN shared_validation | parse and primitive semantics; resource/profile admission; canonical formatting; failure-before-emission boundary
EvaluatedWorkload evaluate_workload(const Workload &workload) {
  RewriteEvaluationResult evaluation =
      evaluate_rewrite_source(workload.source, creation_data(workload));
  if (!evaluation.ok) {
    return EvaluatedWorkload{false, std::move(evaluation), {},
                             "rewrite evaluator rejected workload"};
  }
  std::string canonical_output =
      joined_formatted_output(evaluation.formatted);
  return EvaluatedWorkload{true, std::move(evaluation),
                           std::move(canonical_output), {}};
}

bool evaluation_matches_expected(const Workload &workload,
                                 const EvaluatedWorkload &evaluated) {
  return (workload.expected == ExpectedOutcome::success) == evaluated.ok;
}
// ISSUE33_COMPLEXITY_END

// ISSUE33_COMPLEXITY_BEGIN constants | embeds evaluator-validated canonical result bytes; C literal escaping and chunking; checked stdout write and flush
void append_c_string_literals(std::string &source, std::string_view bytes) {
  constexpr std::size_t bytes_per_literal = 72U;
  if (bytes.empty()) {
    source.append("\"\"");
    return;
  }
  for (std::size_t begin = 0U; begin < bytes.size();
       begin += bytes_per_literal) {
    source.push_back('"');
    const std::size_t end =
        begin + bytes_per_literal < bytes.size() ? begin + bytes_per_literal
                                                 : bytes.size();
    for (std::size_t index = begin; index < end; ++index) {
      const char byte = bytes[index];
      if (byte == '\\' || byte == '"') {
        source.push_back('\\');
        source.push_back(byte);
      } else if (byte == '\n') {
        source.append("\\n");
      } else {
        source.push_back(byte);
      }
    }
    source.append("\"\n");
  }
}

EmissionResult emit_constants(const EvaluatedWorkload &evaluated) {
  std::string source;
  source.append("#include <stddef.h>\n#include <stdio.h>\n\n");
  constexpr std::size_t bytes_per_array = 4000U;
  std::size_t chunk_count = 0U;
  for (std::size_t begin = 0U; begin < evaluated.canonical_output.size();
       begin += bytes_per_array) {
    const std::size_t remaining = evaluated.canonical_output.size() - begin;
    const std::size_t count =
        remaining < bytes_per_array ? remaining : bytes_per_array;
    source.append("static const char output_");
    source.append(std::to_string(chunk_count));
    source.append("[] =\n");
    append_c_string_literals(
        source, std::string_view{evaluated.canonical_output}.substr(begin, count));
    source.append(";\n");
    ++chunk_count;
  }
  source.append("\nint main(void) {\n");
  for (std::size_t chunk = 0U; chunk < chunk_count; ++chunk) {
    source.append("  if (fwrite(output_");
    source.append(std::to_string(chunk));
    source.append(", 1U, sizeof(output_");
    source.append(std::to_string(chunk));
    source.append(") - 1U, stdout) != sizeof(output_");
    source.append(std::to_string(chunk));
    source.append(") - 1U) { return 1; }\n");
  }
  source.append(
      "  return fflush(stdout) == EOF ? 1 : 0;\n"
      "}\n");
  return EmissionResult{true, std::move(source), {}};
}
// ISSUE33_COMPLEXITY_END

// ISSUE33_COMPLEXITY_BEGIN flat_runtime | typed Int payload storage; inc and nested-iota loop execution; Int/vector canonical formatting; checked stdout failures
std::string flat_runtime_prefix() {
  return std::string{
      "#include <inttypes.h>\n"
      "#include <stddef.h>\n"
      "#include <stdint.h>\n"
      "#include <stdio.h>\n\n"
      "int write_text(const char *text) {\n"
      "  return fputs(text, stdout) == EOF ? 1 : 0;\n"
      "}\n\n"
      "int write_int(int64_t value) {\n"
      "  return printf(\"%\" PRId64, value) < 0 ? 1 : 0;\n"
      "}\n\n"
      "int print_incremented_vector(const int64_t *values, size_t count, int64_t increment) {\n"
      "  if (write_text(\"(\") != 0) { return 1; }\n"
      "  for (size_t index = 0U; index < count; ++index) {\n"
      "    if (index != 0U && write_text(\" \") != 0) { return 1; }\n"
      "    if (write_int(values[index] + increment) != 0) { return 1; }\n"
      "  }\n"
      "  return write_text(\")\\n\");\n"
      "}\n\n"
      "int print_iota_vector(uint64_t count, int64_t increment) {\n"
      "  if (write_text(\"(\") != 0) { return 1; }\n"
      "  for (uint64_t index = UINT64_C(0); index < count; ++index) {\n"
      "    if (index != UINT64_C(0) && write_text(\" \") != 0) { return 1; }\n"
      "    if (write_int((int64_t)index + increment) != 0) { return 1; }\n"
      "  }\n"
      "  return write_text(\")\\n\");\n"
      "}\n\n"};
}

EmissionResult emit_flat_runtime(const Workload &workload,
                                 const EvaluatedWorkload &evaluated) {
  std::string source = flat_runtime_prefix();
  if (workload.form == WorkloadForm::scalar_or_empty) {
    source.append("int main(void) {\n  const int status = write_text(");
    append_c_string_literals(source, evaluated.canonical_output);
    source.append(
        ");\n  if (status != 0) { return status; }\n"
        "  return fflush(stdout) == EOF ? 1 : 0;\n}\n");
    return EmissionResult{true, std::move(source), {}};
  }
  if (workload.form == WorkloadForm::arbitrary_lifted_int_vector) {
    source.append(
        "static const int64_t arbitrary_input[] = {INT64_C(7), -INT64_C(3), INT64_C(11), INT64_C(0)};\n\n"
        "int main(void) {\n"
        "  const int status = print_incremented_vector(arbitrary_input, 4U, INT64_C(1));\n"
        "  if (status != 0) { return status; }\n"
        "  return fflush(stdout) == EOF ? 1 : 0;\n"
        "}\n");
    return EmissionResult{true, std::move(source), {}};
  }
  source.append("int main(void) {\n  const int status = print_iota_vector(UINT64_C(");
  source.append(std::to_string(workload.size));
  source.append(
      "), INT64_C(3));\n  if (status != 0) { return status; }\n"
      "  return fflush(stdout) == EOF ? 1 : 0;\n}\n");
  return EmissionResult{true, std::move(source), {}};
}
// ISSUE33_COMPLEXITY_END

// ISSUE33_COMPLEXITY_BEGIN hybrid | exact 1000-element materialization threshold; result-size accounting; dispatch between constants and flat runtime semantics
std::size_t materialized_vector_elements(
    const RewriteEvaluationResult &evaluation) {
  std::size_t total = 0U;
  for (const Value &value : evaluation.values) {
    if (value.container == ContainerKind::vector) {
      std::size_t length = 0U;
      const ValueValidationResult valid = value_length(value, length);
      if (!valid.ok || total > std::numeric_limits<std::size_t>::max() - length) {
        return std::numeric_limits<std::size_t>::max();
      }
      total += length;
    }
  }
  return total;
}

EmissionResult emit_hybrid(const Workload &workload,
                           const EvaluatedWorkload &evaluated) {
  const std::size_t elements =
      materialized_vector_elements(evaluated.evaluation);
  if (elements <= hybrid_vector_element_threshold) {
    return emit_constants(evaluated);
  }
  return emit_flat_runtime(workload, evaluated);
}
// ISSUE33_COMPLEXITY_END

EmissionResult emit_candidate(std::string_view strategy,
                              const Workload &workload,
                              const EvaluatedWorkload &evaluated) {
  if (strategy == "constants") {
    return emit_constants(evaluated);
  }
  if (strategy == "flat") {
    return emit_flat_runtime(workload, evaluated);
  }
  if (strategy == "hybrid") {
    return emit_hybrid(workload, evaluated);
  }
  return EmissionResult{false, {}, "unknown strategy"};
}

bool write_stdout(std::string_view bytes) {
  return std::fwrite(bytes.data(), 1U, bytes.size(), stdout) == bytes.size() &&
         std::fflush(stdout) != EOF;
}

void write_stderr(std::string_view message) {
  static_cast<void>(
      std::fwrite(message.data(), 1U, message.size(), stderr));
  static_cast<void>(std::fputc('\n', stderr));
}

int manifest_command() {
  std::string manifest{"workload_id,workload_size,expected_outcome\n"};
  for (const Workload &workload : workloads) {
    manifest.append(workload.identifier);
    manifest.push_back(',');
    manifest.append(std::to_string(workload.size));
    manifest.push_back(',');
    manifest.append(expected_outcome_name(workload.expected));
    manifest.push_back('\n');
  }
  return write_stdout(manifest) ? 0 : 1;
}

int oracle_command(const Workload &workload) {
  EvaluatedWorkload evaluated = evaluate_workload(workload);
  const bool expected = evaluation_matches_expected(workload, evaluated);
  if (!evaluated.ok) {
    release_rewrite_evaluation_result(evaluated.evaluation);
    write_stderr(expected ? "expected rewrite rejection" : evaluated.error);
    return expected ? 2 : 1;
  }
  const bool written = write_stdout(evaluated.canonical_output);
  release_rewrite_evaluation_result(evaluated.evaluation);
  return written ? 0 : 1;
}

int emit_command(std::string_view strategy, const Workload &workload) {
  EvaluatedWorkload evaluated = evaluate_workload(workload);
  if (!evaluation_matches_expected(workload, evaluated)) {
    release_rewrite_evaluation_result(evaluated.evaluation);
    write_stderr("rewrite result did not match workload expectation");
    return 1;
  }
  if (!evaluated.ok) {
    release_rewrite_evaluation_result(evaluated.evaluation);
    write_stderr("expected rewrite rejection before C emission");
    return 2;
  }
  EmissionResult emitted = emit_candidate(strategy, workload, evaluated);
  release_rewrite_evaluation_result(evaluated.evaluation);
  if (!emitted.ok) {
    write_stderr(emitted.error);
    return 1;
  }
  return write_stdout(emitted.source) ? 0 : 1;
}

int run(int argument_count, char **arguments) {
  if (argument_count == 2 && std::string_view{arguments[1]} == "manifest") {
    return manifest_command();
  }
  if (argument_count == 3 && std::string_view{arguments[1]} == "oracle") {
    const Workload *workload = find_workload(arguments[2]);
    if (workload == nullptr) {
      write_stderr("unknown workload");
      return 1;
    }
    return oracle_command(*workload);
  }
  if (argument_count == 4 && std::string_view{arguments[1]} == "emit") {
    const Workload *workload = find_workload(arguments[3]);
    if (workload == nullptr) {
      write_stderr("unknown workload");
      return 1;
    }
    return emit_command(arguments[2], *workload);
  }
  write_stderr("usage: bennu_issue33_prototype manifest | oracle WORKLOAD | emit STRATEGY WORKLOAD");
  return 1;
}

} // namespace issue33
} // namespace bennu

int main(int argument_count, char **arguments) {
  return bennu::issue33::run(argument_count, arguments);
}
