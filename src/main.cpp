#include "bennu/c_emitter.hpp"
#include "bennu/evaluator.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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

int create_exclusive_file(const char *path) {
#ifdef _WIN32
  return _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
               _S_IREAD | _S_IWRITE);
#else
  return open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
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

bool replace_file(std::string_view temporary_path, std::string_view output_path) {
  const std::string temporary(temporary_path);
  const std::string output(output_path);
#ifdef _WIN32
  return MoveFileExA(temporary.c_str(), output.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return std::rename(temporary.c_str(), output.c_str()) == 0;
#endif
}

bool write_file_atomically(std::string_view path, std::string_view contents) {
  const std::string path_string(path);
  std::string temporary_path;
  int descriptor = -1;
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    temporary_path = path_string + ".tmp";
    if (attempt != 0) {
      temporary_path += '.' + std::to_string(attempt);
    }
    descriptor = create_exclusive_file(temporary_path.c_str());
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
    std::remove(temporary_path.c_str());
    return false;
  }
  const bool write_failed =
      std::fwrite(contents.data(), 1, contents.size(), output) != contents.size();
  const bool close_failed = std::fclose(output) != 0;
  if (write_failed || close_failed) {
    std::remove(temporary_path.c_str());
    return false;
  }
  if (!replace_file(temporary_path, path_string)) {
    std::remove(temporary_path.c_str());
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
    std::cerr << "error: subcommand '" << argument << "' is not implemented\n";
    return 1;
  }

  std::cerr << "error: unknown subcommand '" << argument << "'\n";
  return 1;
}
