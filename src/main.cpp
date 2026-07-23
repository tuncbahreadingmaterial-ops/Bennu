#include "bennu/c_emitter.hpp"
#include "bennu/evaluator.hpp"
#include "bennu/native_builder.hpp"
#include "bennu/path_encoding.hpp"
#include "cli_output.hpp"
#include "runner_arguments.hpp"
#include "bennu_version.hpp"

#include <array>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

std::string_view error_kind_name(bennu::ErrorKind kind) {
  switch (kind) {
  case bennu::ErrorKind::none:
    return "none";
  case bennu::ErrorKind::invalid_byte:
    return "InvalidByte";
  case bennu::ErrorKind::malformed_literal:
    return "MalformedLiteral";
  case bennu::ErrorKind::literal_range_error:
    return "LiteralRangeError";
  case bennu::ErrorKind::syntax_error:
    return "SyntaxError";
  case bennu::ErrorKind::unknown_name:
    return "UnknownPrimitive";
  case bennu::ErrorKind::invalid_parameter_declaration:
    return "ParameterError";
  case bennu::ErrorKind::type_mismatch:
    return "TypeError";
  case bennu::ErrorKind::empty_expression:
    return "EmptyExpression";
  case bennu::ErrorKind::arity_error:
    return "ArityError";
  case bennu::ErrorKind::argument_error:
    return "ArgumentError";
  case bennu::ErrorKind::shape_mismatch:
    return "ShapeMismatch";
  case bennu::ErrorKind::invalid_execution_profile:
    return "InvalidExecutionProfile";
  case bennu::ErrorKind::invalid_primitive_table:
    return "InvalidPrimitiveTable";
  case bennu::ErrorKind::resource_error:
    return "ResourceError";
  case bennu::ErrorKind::domain_error:
    return "DomainError";
  case bennu::ErrorKind::formatting_error:
    return "FormattingError";
  }
  return "unknown error";
}

void write_diagnostic(std::string_view source_name, const bennu::Error &error) {
  std::cerr << source_name << ':' << error.location.line << ':'
            << error.location.column << ": " << error_kind_name(error.kind)
            << ": " << error.message << '\n';
}

std::string_view argument_reason_name(bennu::ArgumentErrorReason reason) {
  switch (reason) {
  case bennu::ArgumentErrorReason::missing:
    return "missing";
  case bennu::ArgumentErrorReason::extra:
    return "extra";
  case bennu::ArgumentErrorReason::invalid_literal:
    return "invalid_literal";
  case bennu::ArgumentErrorReason::out_of_range:
    return "out_of_range";
  case bennu::ArgumentErrorReason::invalid_typed_value:
    return "invalid_typed_value";
  case bennu::ArgumentErrorReason::container_mismatch:
    return "container_mismatch";
  case bennu::ArgumentErrorReason::type_mismatch:
    return "type_mismatch";
  }
  return "invalid_literal";
}

std::string_view scalar_type_name(bennu::ScalarType type) {
  switch (type) {
  case bennu::ScalarType::boolean:
    return "Bool";
  case bennu::ScalarType::integer:
    return "Int";
  case bennu::ScalarType::double_precision:
    return "Double";
  }
  return "-";
}

std::string_view argument_container_name(bennu::ArgumentContainer container) {
  switch (container) {
  case bennu::ArgumentContainer::unknown:
    return "unknown";
  case bennu::ArgumentContainer::scalar:
    return "scalar";
  case bennu::ArgumentContainer::vector:
    return "vector";
  }
  return "unknown";
}

std::string_view argument_scalar_type_name(bennu::ArgumentScalarType type) {
  switch (type) {
  case bennu::ArgumentScalarType::unknown:
    return "unknown";
  case bennu::ArgumentScalarType::boolean:
    return "Bool";
  case bennu::ArgumentScalarType::integer:
    return "Int";
  case bennu::ArgumentScalarType::double_precision:
    return "Double";
  }
  return "unknown";
}

std::string_view value_invariant_name(bennu::ValueInvariant invariant) {
  switch (invariant) {
  case bennu::ValueInvariant::unknown_container:
    return "unknown_container";
  case bennu::ValueInvariant::unknown_scalar_type:
    return "unknown_scalar_type";
  case bennu::ValueInvariant::inactive_scalar_field:
    return "inactive_scalar_field";
  case bennu::ValueInvariant::noncanonical_nan:
    return "noncanonical_nan";
  case bennu::ValueInvariant::none:
  case bennu::ValueInvariant::inactive_vector_payload:
  case bennu::ValueInvariant::invalid_boolean_element:
    break;
  }
  return "-";
}

void append_unsigned(std::string &output, std::size_t value) {
  std::array<char, 32> digits{};
  const std::to_chars_result converted =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  output.append(digits.data(), converted.ptr);
}

void append_position(std::string &output, bennu::SourceLocation position) {
  append_unsigned(output, position.offset);
  output.push_back(':');
  append_unsigned(output, position.line);
  output.push_back(':');
  append_unsigned(output, position.column);
}

void append_optional(std::string &output,
                     const std::optional<std::string> &value) {
  output += value.has_value() ? *value : "-";
}

void write_argument_diagnostic(const bennu::ArgumentErrorContext &argument) {
  std::string record = "bennu_argument_error reason=";
  record += argument_reason_name(argument.reason);
  record += " required_count=";
  append_unsigned(record, argument.required_count);
  record += " supplied_count=";
  append_unsigned(record, argument.supplied_count);
  record += " position=";
  append_unsigned(record, argument.position);
  record += " parameter_name=";
  append_optional(record, argument.parameter_name);
  record += " expected_type=";
  record += argument.expected_type.has_value()
                ? scalar_type_name(*argument.expected_type)
                : "-";
  record += " declaration_span=";
  if (argument.declaration_span.has_value()) {
    append_position(record, argument.declaration_span->begin);
    record.push_back('-');
    append_position(record, argument.declaration_span->end);
  } else {
    record.push_back('-');
  }
  record += " actual_container=";
  record += argument.actual_container.has_value()
                ? argument_container_name(*argument.actual_container)
                : "-";
  record += " actual_type=";
  record += argument.actual_type.has_value()
                ? argument_scalar_type_name(*argument.actual_type)
                : "-";
  record += " invalid_value_invariant=";
  record += argument.invalid_value_invariant.has_value()
                ? value_invariant_name(*argument.invalid_value_invariant)
                : "-";
  record.push_back('\n');
  std::cerr << record;
}

std::string_view formatting_reason_name(bennu::ValueFormatError reason) {
  switch (reason) {
  case bennu::ValueFormatError::invalid_value:
    return "invalid_value";
  case bennu::ValueFormatError::conversion_failure:
    return "conversion_failure";
  case bennu::ValueFormatError::none:
    break;
  }
  return "conversion_failure";
}

void write_formatting_diagnostic(
    const bennu::FormattingErrorContext &formatting) {
  std::string record = "bennu_formatting_error reason=";
  record += formatting_reason_name(formatting.reason);
  record += " root_position=";
  append_unsigned(record, formatting.root_position);
  record += " root_span=";
  append_position(record, formatting.root_span.begin);
  record.push_back('-');
  append_position(record, formatting.root_span.end);
  record += " invalid_value_invariant=";
  record += formatting.invalid_value_invariant.has_value()
                ? value_invariant_name(*formatting.invalid_value_invariant)
                : "-";
  record.push_back('\n');
  std::cerr << record;
}

int report_stdout_failure() {
  std::cerr << "error: unable to write stdout\n";
  return 1;
}

bool format_output_value(const bennu::Value &value, std::string &output) {
  bennu::ValueFormattingResult formatted = bennu::format_value(value);
  if (!formatted.ok) {
    std::cerr << "error: evaluator produced an invalid value\n";
    return false;
  }
  output = formatted.formatted;
  output += '\n';
  return true;
}

bool is_blank_line(std::string_view line) {
  for (const char character : line) {
    if (character != ' ' && character != '\t') {
      return false;
    }
  }
  return true;
}

struct FileReadResult {
  bool ok;
  std::string source;
};

FileReadResult read_source_file(std::string_view path) {
  const bennu::PathFromUtf8Result converted =
      bennu::path_for_io_from_utf8(path);
  if (!converted.ok) {
    return FileReadResult{false, {}};
  }
#ifdef _WIN32
  std::FILE *input = _wfopen(converted.path.c_str(), L"rb");
#else
  std::FILE *input = std::fopen(converted.path.c_str(), "rb");
#endif
  if (input == nullptr) {
    return FileReadResult{false, {}};
  }

  std::string source;
  std::array<char, 8192> buffer{};
  while (true) {
    const std::size_t count =
        std::fread(buffer.data(), 1, buffer.size(), input);
    source.append(buffer.data(), count);
    if (count != buffer.size()) {
      const bool read_failed = std::ferror(input) != 0;
      const bool close_failed = std::fclose(input) != 0;
      if (read_failed || close_failed) {
        return FileReadResult{false, {}};
      }
      return FileReadResult{true, std::move(source)};
    }
  }
}

struct SourceOutputIdentity {
  bool ok;
  bool aliases;
};

SourceOutputIdentity
check_source_output_identity(std::string_view source_path,
                             std::string_view output_path) {
  const bennu::PathFromUtf8Result source_result =
      bennu::path_for_io_from_utf8(source_path);
  const bennu::PathFromUtf8Result output_result =
      bennu::path_for_io_from_utf8(output_path);
  if (!source_result.ok || !output_result.ok) {
    return SourceOutputIdentity{false, false};
  }
  const std::filesystem::path &source = source_result.path;
  const std::filesystem::path &output = output_result.path;
  std::error_code error;
  const std::filesystem::path normalized_source =
      std::filesystem::absolute(source, error).lexically_normal();
  if (error) {
    return SourceOutputIdentity{false, false};
  }
  const std::filesystem::path normalized_output =
      std::filesystem::absolute(output, error).lexically_normal();
  if (error) {
    return SourceOutputIdentity{false, false};
  }
  if (normalized_source == normalized_output) {
    return SourceOutputIdentity{true, true};
  }

  error.clear();
  const bool aliases = std::filesystem::equivalent(source, output, error);
  if (!error) {
    return SourceOutputIdentity{true, aliases};
  }

  error.clear();
  const bool output_exists = std::filesystem::exists(output, error);
  if (error || output_exists) {
    return SourceOutputIdentity{false, false};
  }
  return SourceOutputIdentity{true, false};
}

bool reject_source_output_alias(std::string_view source_path,
                                std::string_view output_path) {
  const SourceOutputIdentity identity =
      check_source_output_identity(source_path, output_path);
  if (!identity.ok) {
    std::cerr << "error: unable to determine source/output path identity\n";
    return true;
  }
  if (identity.aliases) {
    std::cerr
        << "error: source/output alias: output path refers to input source\n";
    return true;
  }
  return false;
}

int run_file(std::string_view path,
             std::span<const std::string_view> arguments) {
  FileReadResult loaded = read_source_file(path);
  if (!loaded.ok) {
    std::cerr << path << ":1:1: file error: unable to read source\n";
    return 1;
  }

  const bennu::RunnerEvaluationResult result =
      bennu::evaluate_runner_source(loaded.source, arguments);
  if (!result.ok) {
    if (result.error.kind == bennu::ErrorKind::argument_error &&
        result.error.argument.has_value()) {
      write_argument_diagnostic(*result.error.argument);
    } else if (result.error.kind == bennu::ErrorKind::formatting_error &&
               result.error.formatting.has_value()) {
      write_formatting_diagnostic(*result.error.formatting);
    } else {
      write_diagnostic(path, result.error);
    }
    return 1;
  }

  std::string pending_output;
  for (const std::string &formatted : result.formatted) {
    pending_output += formatted;
    pending_output.push_back('\n');
  }
  const bennu_cli::OutputPublicationResult published =
      bennu_cli::publish_stdout(std::cout, pending_output);
  if (!published.ok) {
    std::cerr << bennu_cli::output_error_record(published);
    return 1;
  }
  return 0;
}

int create_exclusive_file(const std::filesystem::path &path) {
#ifdef _WIN32
  return _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                _S_IREAD | _S_IWRITE);
#else
  return open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
}

std::FILE *open_file_stream(int descriptor) {
#ifdef _WIN32
  return _fdopen(descriptor, "wb");
#else
  return fdopen(descriptor, "wb");
#endif
}

void close_file_descriptor(int descriptor) {
#ifdef _WIN32
  _close(descriptor);
#else
  close(descriptor);
#endif
}

bool replace_file(const std::filesystem::path &temporary,
                  const std::filesystem::path &output) {
#ifdef _WIN32
  return MoveFileExW(temporary.c_str(), output.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return std::rename(temporary.c_str(), output.c_str()) == 0;
#endif
}

bool write_file_atomically(std::string_view path, std::string_view contents) {
  const bennu::PathFromUtf8Result converted =
      bennu::path_for_io_from_utf8(path);
  if (!converted.ok) {
    return false;
  }
  const std::filesystem::path &output_path = converted.path;
  std::filesystem::path temporary_path;
  int descriptor = -1;
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    temporary_path = output_path;
    temporary_path += ".tmp";
    if (attempt != 0) {
      temporary_path += '.' + std::to_string(attempt);
    }
    descriptor = create_exclusive_file(temporary_path);
    if (descriptor >= 0) {
      break;
    }
    if (errno != EEXIST) {
      return false;
    }
  }
  if (descriptor < 0) {
    return false;
  }

  std::FILE *output = open_file_stream(descriptor);
  if (output == nullptr) {
    close_file_descriptor(descriptor);
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    return false;
  }
  const bool write_failed =
      std::fwrite(contents.data(), 1, contents.size(), output) != contents.size();
  const bool close_failed = std::fclose(output) != 0;
  if (write_failed || close_failed) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    return false;
  }
  if (!replace_file(temporary_path, output_path)) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    return false;
  }
  return true;
}

int emit_c_file(std::string_view source_path, std::string_view output_path) {
  FileReadResult loaded = read_source_file(source_path);
  if (!loaded.ok) {
    std::cerr << source_path << ":1:1: file error: unable to read source\n";
    return 1;
  }
  if (reject_source_output_alias(source_path, output_path)) {
    return 1;
  }

  bennu::CEmissionResult emitted = bennu::emit_c_source(loaded.source);
  if (!emitted.ok) {
    write_diagnostic(source_path, emitted.error);
    return 1;
  }
  if (!write_file_atomically(output_path, emitted.source)) {
    std::cerr << output_path << ":1:1: file error: unable to write output\n";
    return 1;
  }
  return 0;
}

struct EnvironmentCompiler {
  bool ok;
  std::string value;
};

EnvironmentCompiler read_environment_compiler() {
#ifdef _WIN32
  const wchar_t *environment = _wgetenv(L"CC");
  if (environment == nullptr) {
    return EnvironmentCompiler{true, {}};
  }
  bennu::Utf8StringResult converted = bennu::wide_to_utf8(environment);
  return EnvironmentCompiler{converted.ok, std::move(converted.text)};
#else
  const char *environment = std::getenv("CC");
  return EnvironmentCompiler{
      true, environment == nullptr ? std::string{} : std::string(environment)};
#endif
}

int build_native_file(std::string_view source_path, std::string_view output_path,
                      std::string_view compiler) {
  FileReadResult loaded = read_source_file(source_path);
  if (!loaded.ok) {
    std::cerr << source_path << ":1:1: file error: unable to read source\n";
    return 1;
  }
  if (reject_source_output_alias(source_path, output_path)) {
    return 1;
  }

  bennu::CEmissionResult emitted = bennu::emit_c_source(loaded.source);
  if (!emitted.ok) {
    write_diagnostic(source_path, emitted.error);
    return 1;
  }

  const EnvironmentCompiler environment_compiler = read_environment_compiler();
  if (!environment_compiler.ok) {
    std::cerr << "error: native build: unable to decode CC as Unicode\n";
    return 1;
  }
  const bennu::NativeBuildResult built = bennu::build_native(
      bennu::NativeBuildRequest{emitted.source, output_path, compiler,
                                environment_compiler.value});
  if (!built.ok) {
    std::cerr << "error: native build: " << built.error << '\n';
    return 1;
  }
  return 0;
}

int run_repl() {
  std::string line;
  while (true) {
    if (!bennu_cli::write_stdout(std::cout, "> ") ||
        !bennu_cli::flush_stdout(std::cout)) {
      return report_stdout_failure();
    }
    if (!std::getline(std::cin, line)) {
      return 0;
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (is_blank_line(line)) {
      continue;
    }

    const bennu::ValueResult result = bennu::evaluate_expression(line);
    if (!result.ok) {
      write_diagnostic("<repl>", result.error);
      continue;
    }
    std::string output;
    if (!format_output_value(result.value, output)) {
      return 1;
    }
    if (!bennu_cli::write_stdout(std::cout, output)) {
      return report_stdout_failure();
    }
  }
}

int run_cli(const std::vector<std::string> &argv) {
  const std::size_t argc = argv.size();
  if (argc == 1) {
    std::cerr << "error: expected a subcommand or --help\n";
    return 1;
  }

  const std::string_view argument = argv[1];

  if (argc == 2 && argument == "--help") {
    constexpr std::string_view help =
        "Usage: bennu <command> [arguments]\n"
        "       bennu --help\n"
        "       bennu --version\n"
        "\n"
        "Commands:\n"
        "  repl    Start an interactive Bennu session\n"
        "  run <source> [-- <arguments...>]\n"
        "          Run a Bennu source file\n"
        "  emit-c  Emit C source for a Bennu source file\n"
        "  build   Build a Bennu source file\n";
    if (!bennu_cli::write_stdout(std::cout, help) ||
        !bennu_cli::flush_stdout(std::cout)) {
      return report_stdout_failure();
    }
    return 0;
  }

  if (argc == 2 && argument == "--version") {
    std::string output = "bennu ";
    output += bennu_build::version;
    output += '\n';
    if (!bennu_cli::write_stdout(std::cout, output) ||
        !bennu_cli::flush_stdout(std::cout)) {
      return report_stdout_failure();
    }
    return 0;
  }

  if (argument.starts_with('-')) {
    std::cerr << "error: unknown option '" << argument << "'\n";
    return 1;
  }

  if (argument == "run") {
    if (argc == 2) {
      std::cerr << "error: expected one source path after 'run'\n";
      return 1;
    }
    if (argc > 3 && std::string_view(argv[3]) != "--") {
      std::cerr << "error: expected 'run <source> [-- <arguments...>]'\n";
      return 1;
    }
    std::vector<std::string_view> script_arguments;
    if (argc > 3) {
      script_arguments.reserve(argc - 4U);
      for (std::size_t index = 4U; index < argc; ++index) {
        script_arguments.push_back(argv[index]);
      }
    }
    return run_file(argv[2], script_arguments);
  }

  if (argument == "repl") {
    if (argc != 2) {
      std::cerr << "error: 'repl' does not accept arguments\n";
      return 1;
    }
    return run_repl();
  }

  if (argument == "emit-c") {
    if (argc == 2) {
      std::cerr << "error: expected a source path after 'emit-c'\n";
      return 1;
    }
    if (argc != 5 || std::string_view(argv[3]) != "-o") {
      std::cerr << "error: expected 'emit-c <source> -o <output>'\n";
      return 1;
    }
    return emit_c_file(argv[2], argv[4]);
  }

  if (argument == "build") {
    if (argc == 2) {
      std::cerr << "error: expected a source path after 'build'\n";
      return 1;
    }
    if ((argc != 5 && argc != 7) || std::string_view(argv[3]) != "-o" ||
        (argc == 7 && std::string_view(argv[5]) != "--cc")) {
      std::cerr << "error: expected 'build <source> -o <output> [--cc <compiler>]'\n";
      return 1;
    }
    const std::string_view compiler =
        argc == 7 ? std::string_view(argv[6]) : std::string_view{};
    if (argc == 7 && compiler.empty()) {
      std::cerr << "error: --cc requires a nonempty compiler\n";
      return 1;
    }
    return build_native_file(argv[2], argv[4], compiler);
  }

  std::cerr << "error: unknown subcommand '" << argument << "'\n";
  return 1;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> argv;
  argv.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    bennu::Utf8StringResult converted = bennu::wide_to_utf8(wide_argv[index]);
    if (!converted.ok) {
      std::cerr << "error: unable to decode Unicode command line\n";
      return 1;
    }
    argv.push_back(std::move(converted.text));
  }
  return run_cli(argv);
}
#else
int main(int argc, char **raw_argv) {
  std::vector<std::string> argv;
  argv.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    argv.emplace_back(raw_argv[index]);
  }
  return run_cli(argv);
}
#endif
