#include "bennu/path_encoding.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

struct EnvironmentValue {
  bool ok;
  bool present;
  std::string value;
};

#ifdef _WIN32
EnvironmentValue read_environment(const wchar_t *name) {
  const wchar_t *value = _wgetenv(name);
  if (value == nullptr) {
    return EnvironmentValue{true, false, {}};
  }
  bennu::Utf8StringResult converted = bennu::wide_to_utf8(value);
  return EnvironmentValue{converted.ok, true, std::move(converted.text)};
}
#else
EnvironmentValue read_environment(const char *name) {
  const char *value = std::getenv(name);
  return EnvironmentValue{true, value != nullptr,
                          value == nullptr ? std::string{} : std::string(value)};
}
#endif

std::FILE *open_output(std::string_view path) {
  const bennu::PathFromUtf8Result converted =
      bennu::path_for_io_from_utf8(path);
  if (!converted.ok) {
    return nullptr;
  }
#ifdef _WIN32
  return _wfopen(converted.path.c_str(), L"wb");
#else
  return std::fopen(converted.path.c_str(), "wb");
#endif
}

bool write_bytes(std::string_view path, std::string_view bytes) {
  std::FILE *output = open_output(path);
  if (output == nullptr) {
    return false;
  }
  const bool write_failed =
      std::fwrite(bytes.data(), 1, bytes.size(), output) != bytes.size();
  const bool close_failed = std::fclose(output) != 0;
  return !write_failed && !close_failed;
}

int run_fake_compiler(const std::vector<std::string> &arguments) {
#ifdef _WIN32
  const EnvironmentValue trace_path = read_environment(L"BENNU_FAKE_CC_TRACE");
  const EnvironmentValue mode_value = read_environment(L"BENNU_FAKE_CC_MODE");
#else
  const EnvironmentValue trace_path = read_environment("BENNU_FAKE_CC_TRACE");
  const EnvironmentValue mode_value = read_environment("BENNU_FAKE_CC_MODE");
#endif
  if (!trace_path.ok || !mode_value.ok) {
    return 29;
  }
  if (trace_path.present) {
    std::string trace;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      trace += std::to_string(index) + ':' +
               std::to_string(arguments[index].size()) + ':' + arguments[index] +
               '\n';
    }
    if (!write_bytes(trace_path.value, trace)) {
      return 29;
    }
  }

  const std::string_view mode = mode_value.present
                                    ? std::string_view(mode_value.value)
                                    : std::string_view("success");
  if (mode == "fail") {
    std::cerr << "fake compiler diagnostic\n";
    return 23;
  }
  if (mode == "terminate") {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0xC000013AUL);
#else
    std::raise(SIGTERM);
#endif
    return 24;
  }
  if (mode == "no-output") {
    return 0;
  }

  std::string output_path;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument(arguments[index]);
    if (argument == "-o" && index + 1 < arguments.size()) {
      output_path = arguments[index + 1];
      break;
    }
    if (argument.starts_with("/Fe:")) {
      output_path = std::string(argument.substr(4));
      break;
    }
  }
  if (output_path.empty()) {
    std::cerr << "fake compiler did not receive an output argument\n";
    return 25;
  }

  constexpr std::string_view bytes = "fake native output\n";
  if (!write_bytes(output_path, bytes)) {
    std::cerr << "fake compiler could not create output\n";
    return 26;
  }
#ifndef _WIN32
  if (mode != "non-executable-output" && chmod(output_path.c_str(), 0700) != 0) {
    return 28;
  }
#endif
  return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_arguments) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    bennu::Utf8StringResult converted =
        bennu::wide_to_utf8(wide_arguments[index]);
    if (!converted.ok) {
      return 29;
    }
    arguments.push_back(std::move(converted.text));
  }
  return run_fake_compiler(arguments);
}
#else
int main(int argc, char **raw_arguments) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    arguments.emplace_back(raw_arguments[index]);
  }
  return run_fake_compiler(arguments);
}
#endif