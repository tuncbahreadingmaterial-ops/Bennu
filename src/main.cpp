#include "bennu/evaluator.hpp"

#include <array>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string_view error_kind_name(bennu::ErrorKind kind) {
  switch (kind) {
  case bennu::ErrorKind::none:
    return "none";
  case bennu::ErrorKind::illegal_character:
    return "illegal character";
  case bennu::ErrorKind::malformed_integer:
    return "malformed integer";
  case bennu::ErrorKind::integer_out_of_range:
    return "integer out of range";
  case bennu::ErrorKind::unknown_name:
    return "unknown name";
  case bennu::ErrorKind::missing_argument:
    return "missing argument";
  case bennu::ErrorKind::expected_whitespace:
    return "expected whitespace";
  case bennu::ErrorKind::trailing_token:
    return "trailing token";
  case bennu::ErrorKind::integer_overflow:
    return "integer overflow";
  case bennu::ErrorKind::type_mismatch:
    return "type mismatch";
  case bennu::ErrorKind::allocation_limit_exceeded:
    return "allocation limit exceeded";
  case bennu::ErrorKind::empty_expression:
    return "empty expression";
  case bennu::ErrorKind::invalid_program:
    return "invalid program";
  }
  return "unknown error";
}

void write_diagnostic(std::string_view source_name, const bennu::Error &error) {
  std::cerr << source_name << ':' << error.location.line << ':'
            << error.location.column << ": " << error_kind_name(error.kind)
            << ": " << error.message << '\n';
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
  const std::string path_string(path);
  std::FILE *input = std::fopen(path_string.c_str(), "rb");
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

int run_file(std::string_view path) {
  FileReadResult loaded = read_source_file(path);
  if (!loaded.ok) {
    std::cerr << path << ":1:1: file error: unable to read source\n";
    return 1;
  }

  const bennu::ProgramResult result = bennu::evaluate_source(loaded.source);
  if (!result.ok) {
    write_diagnostic(path, result.error);
    return 1;
  }

  for (const bennu::Value &value : result.values) {
    std::cout << ">>" << bennu::format_value(value) << '\n';
  }
  return 0;
}

int run_repl() {
  std::string line;
  while (true) {
    std::cout << "> " << std::flush;
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
    std::cout << ">>" << bennu::format_value(result.value) << '\n';
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 1) {
    std::cerr << "error: expected a subcommand or --help\n";
    return 1;
  }

  const std::string_view argument = argv[1];

  if (argc == 2 && argument == "--help") {
    std::cout << "Usage: bennu <command> [arguments]\n"
                 "       bennu --help\n"
                 "\n"
                 "Commands:\n"
                 "  repl    Start an interactive Bennu session\n"
                 "  run     Run a Bennu source file\n"
                 "  emit-c  Emit C source for a Bennu source file\n"
                 "  build   Build a Bennu source file\n";
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
    if (argc != 3) {
      std::cerr << "error: 'run' expects exactly one source path\n";
      return 1;
    }
    return run_file(argv[2]);
  }

  if (argument == "repl") {
    if (argc != 2) {
      std::cerr << "error: 'repl' does not accept arguments\n";
      return 1;
    }
    return run_repl();
  }

  if (argument == "emit-c" || argument == "build") {
    std::cerr << "error: subcommand '" << argument << "' is not implemented\n";
    return 1;
  }

  std::cerr << "error: unknown subcommand '" << argument << "'\n";
  return 1;
}
