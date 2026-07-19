#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

int main(int argc, char **argv) {
  const char *trace_path = std::getenv("BENNU_FAKE_CC_TRACE");
  if (trace_path != nullptr) {
    std::ofstream trace(trace_path, std::ios::binary | std::ios::trunc);
    for (int index = 0; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      trace << index << ':' << argument.size() << ':' << argument << '\n';
    }
  }

  const char *mode_value = std::getenv("BENNU_FAKE_CC_MODE");
  const std::string_view mode = mode_value == nullptr ? "success" : mode_value;
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
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "-o" && index + 1 < argc) {
      output_path = argv[index + 1];
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

  std::FILE *output = std::fopen(output_path.c_str(), "wb");
  if (output == nullptr) {
    std::cerr << "fake compiler could not create output\n";
    return 26;
  }
  constexpr std::string_view bytes = "fake native output\n";
  const bool write_failed =
      std::fwrite(bytes.data(), 1, bytes.size(), output) != bytes.size();
  const bool close_failed = std::fclose(output) != 0;
  if (write_failed || close_failed) {
    return 27;
  }
#ifndef _WIN32
  if (mode != "non-executable-output" &&
      chmod(output_path.c_str(), 0700) != 0) {
    return 28;
  }
#endif
  return 0;
}
