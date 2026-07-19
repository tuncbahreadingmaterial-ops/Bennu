#include "bennu/native_builder.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
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
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bennu {
namespace {

struct ProcessResult {
  bool started;
  bool terminated;
  int exit_code;
  int start_error;
  std::string output;
};

std::string path_string(const std::filesystem::path &path) {
#ifdef _WIN32
  return path.string();
#else
  return path.native();
#endif
}

bool write_bytes(const std::filesystem::path &path, std::string_view bytes) {
  const std::string name = path_string(path);
  std::FILE *file = std::fopen(name.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const bool write_failed =
      std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size();
  const bool close_failed = std::fclose(file) != 0;
  return !write_failed && !close_failed;
}

std::string read_process_output(const std::filesystem::path &path) {
  const std::string name = path_string(path);
  std::FILE *file = std::fopen(name.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::array<char, 4096> buffer{};
  const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), file);
  std::fclose(file);
  return std::string(buffer.data(), count);
}

bool has_path_separator(std::string_view executable) {
  return executable.find('/') != std::string_view::npos ||
         executable.find('\\') != std::string_view::npos;
}

#ifdef _WIN32
bool executable_exists(std::string_view executable) {
  const std::string name(executable);
  if (has_path_separator(executable) || executable.find(':') != std::string_view::npos) {
    const DWORD attributes = GetFileAttributesA(name.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  }
  const DWORD required = SearchPathA(nullptr, name.c_str(), nullptr, 0, nullptr, nullptr);
  return required != 0;
}

std::string quote_windows_argument(std::string_view argument) {
  std::string quoted = "\"";
  std::size_t backslashes = 0;
  for (const char character : argument) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      quoted += '"';
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, '\\');
    backslashes = 0;
    quoted += character;
  }
  quoted.append(backslashes * 2, '\\');
  quoted += '"';
  return quoted;
}

ProcessResult run_process(std::string_view executable,
                          const std::vector<std::string> &arguments,
                          const std::filesystem::path &working_directory,
                          const std::filesystem::path &log_path) {
  const std::string log_name = path_string(log_path);
  HANDLE log = CreateFileA(log_name.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) {
    return ProcessResult{false, false, 0, static_cast<int>(GetLastError()), {}};
  }
  if (SetHandleInformation(log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(log);
    return ProcessResult{false, false, 0, static_cast<int>(error), {}};
  }

  std::string command_line = quote_windows_argument(executable);
  for (const std::string &argument : arguments) {
    command_line += ' ';
    command_line += quote_windows_argument(argument);
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log;
  startup.hStdError = log;
  PROCESS_INFORMATION process{};
  const std::string directory = path_string(working_directory);
  const BOOL created = CreateProcessA(
      nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr,
      directory.c_str(), &startup, &process);
  const DWORD start_error = created != 0 ? ERROR_SUCCESS : GetLastError();
  CloseHandle(log);
  if (created == 0) {
    return ProcessResult{false, false, 0, static_cast<int>(start_error), {}};
  }

  if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
    const DWORD error = GetLastError();
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ProcessResult{false, false, 0, static_cast<int>(error),
                         read_process_output(log_path)};
  }
  DWORD exit_code = 1;
  if (GetExitCodeProcess(process.hProcess, &exit_code) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ProcessResult{false, false, 0, static_cast<int>(error),
                         read_process_output(log_path)};
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  const bool terminated = exit_code >= 0xC0000000UL;
  return ProcessResult{true, terminated, static_cast<int>(exit_code), 0,
                       read_process_output(log_path)};
}
#else
std::string resolve_path_executable(std::string_view executable) {
  if (has_path_separator(executable)) {
    return {};
  }
  const char *path_value = std::getenv("PATH");
  if (path_value == nullptr) {
    return {};
  }
  std::string_view path(path_value);
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t end = path.find(':', begin);
    const std::string_view directory =
        end == std::string_view::npos ? path.substr(begin)
                                      : path.substr(begin, end - begin);
    std::string candidate = directory.empty() ? "." : std::string(directory);
    candidate += '/';
    candidate += executable;
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::status(candidate, error);
    if (!error && std::filesystem::is_regular_file(status) &&
        access(candidate.c_str(), X_OK) == 0) {
      const std::filesystem::path absolute =
          std::filesystem::absolute(std::filesystem::path(candidate), error);
      if (!error) {
        return path_string(absolute);
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return {};
}

bool executable_exists(std::string_view executable) {
  const std::string name(executable);
  if (has_path_separator(executable)) {
    return access(name.c_str(), X_OK) == 0;
  }
  return !resolve_path_executable(executable).empty();
}

ProcessResult run_process(std::string_view executable,
                          const std::vector<std::string> &arguments,
                          const std::filesystem::path &working_directory,
                          const std::filesystem::path &log_path) {
  const std::string log_name = path_string(log_path);
  const int log = open(log_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (log < 0) {
    return ProcessResult{false, false, 0, errno, {}};
  }
  int start_pipe[2] = {-1, -1};
  if (pipe(start_pipe) != 0) {
    const int error = errno;
    close(log);
    return ProcessResult{false, false, 0, error, {}};
  }
  const int descriptor_flags = fcntl(start_pipe[1], F_GETFD);
  if (descriptor_flags < 0 ||
      fcntl(start_pipe[1], F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    const int error = errno;
    close(log);
    close(start_pipe[0]);
    close(start_pipe[1]);
    return ProcessResult{false, false, 0, error, {}};
  }

  const pid_t child = fork();
  if (child < 0) {
    const int error = errno;
    close(log);
    close(start_pipe[0]);
    close(start_pipe[1]);
    return ProcessResult{false, false, 0, error, {}};
  }
  if (child == 0) {
    close(start_pipe[0]);
    const std::string directory = path_string(working_directory);
    if (chdir(directory.c_str()) != 0 || dup2(log, STDOUT_FILENO) < 0 ||
        dup2(log, STDERR_FILENO) < 0) {
      const int error = errno;
      const ssize_t ignored = write(start_pipe[1], &error, sizeof(error));
      static_cast<void>(ignored);
      _exit(127);
    }
    close(log);

    std::string executable_copy(executable);
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(executable_copy.data());
    for (const std::string &argument : arguments) {
      argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    execvp(executable_copy.c_str(), argv.data());
    const int error = errno;
    const ssize_t ignored = write(start_pipe[1], &error, sizeof(error));
    static_cast<void>(ignored);
    _exit(127);
  }

  close(log);
  close(start_pipe[1]);
  int start_error = 0;
  ssize_t start_count = -1;
  do {
    start_count = read(start_pipe[0], &start_error, sizeof(start_error));
  } while (start_count < 0 && errno == EINTR);
  const int read_error = start_count < 0 ? errno : 0;
  close(start_pipe[0]);
  int status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0) {
    return ProcessResult{false, false, 0, errno, read_process_output(log_path)};
  }
  if (start_count < 0) {
    return ProcessResult{false, false, 0, read_error,
                         read_process_output(log_path)};
  }
  if (start_count > 0) {
    return ProcessResult{false, false, 0, start_error,
                         read_process_output(log_path)};
  }
  if (WIFSIGNALED(status)) {
    return ProcessResult{true, true, WTERMSIG(status), 0,
                         read_process_output(log_path)};
  }
  return ProcessResult{true, false, WIFEXITED(status) ? WEXITSTATUS(status) : 1,
                       0, read_process_output(log_path)};
}
#endif

std::string lower_filename(std::string_view compiler) {
  std::string name = std::filesystem::path(std::string(compiler)).filename().string();
  for (char &character : name) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return name;
}

NativePlatform compiler_platform(NativePlatform host,
                                 std::string_view compiler) {
  if (host == NativePlatform::windows_msvc) {
    const std::string name = lower_filename(compiler);
    if (name == "cl" || name == "cl.exe") {
      return NativePlatform::windows_msvc;
    }
  }
  return NativePlatform::gcc_like;
}

bool replace_output(const std::filesystem::path &temporary,
                    const std::filesystem::path &output) {
  const std::string temporary_name = path_string(temporary);
  const std::string output_name = path_string(output);
#ifdef _WIN32
  return MoveFileExA(temporary_name.c_str(), output_name.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return std::rename(temporary_name.c_str(), output_name.c_str()) == 0;
#endif
}

bool reserve_replacement_path(const std::filesystem::path &path) {
  const std::string name = path_string(path);
#ifdef _WIN32
  const int descriptor = _open(name.c_str(),
                               _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                               _S_IREAD | _S_IWRITE);
  if (descriptor < 0) {
    return false;
  }
  if (_close(descriptor) != 0) {
    std::remove(name.c_str());
    return false;
  }
#else
  const int descriptor = open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    return false;
  }
  if (close(descriptor) != 0) {
    std::remove(name.c_str());
    return false;
  }
#endif
  return true;
}

NativeBuildResult failure_with_cleanup(const std::filesystem::path &temporary,
                                       std::string message) {
  std::error_code cleanup_error;
  std::filesystem::remove_all(temporary, cleanup_error);
  if (cleanup_error) {
    message += "; unable to clean isolated build temporary directory";
  }
  return NativeBuildResult{false, std::move(message)};
}

bool is_usable_native_output(const std::filesystem::path &path,
                             std::error_code &error) {
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    return false;
  }
#ifdef _WIN32
  return true;
#else
  const std::string name = path_string(path);
  return access(name.c_str(), X_OK) == 0;
#endif
}

std::string process_failure(const CompilerSelection &selection,
                            const ProcessResult &process) {
  std::string error = "compiler selected from ";
  error += compiler_configuration_name(selection.configuration);
  error += " ('" + selection.executable + "') ";
  if (!process.started) {
    error += "could not be started";
    error += " (OS error " + std::to_string(process.start_error) + ')';
  } else if (process.terminated) {
    error += "was terminated";
  } else {
    error += "exited with status " + std::to_string(process.exit_code);
  }
  if (!process.output.empty()) {
    error += ": ";
    error += process.output;
    while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) {
      error.pop_back();
    }
  }
  return error;
}

} // namespace

NativePlatform native_platform() {
#ifdef _WIN32
  return NativePlatform::windows_msvc;
#else
  return NativePlatform::gcc_like;
#endif
}

std::string_view compiler_configuration_name(CompilerConfiguration configuration) {
  switch (configuration) {
  case CompilerConfiguration::explicit_option:
    return "--cc";
  case CompilerConfiguration::environment:
    return "CC";
  case CompilerConfiguration::fallback:
    return "platform fallback";
  }
  return "unknown configuration";
}

CompilerSelection select_c_compiler(std::string_view explicit_compiler,
                                    std::string_view environment_compiler,
                                    NativePlatform platform) {
  if (!explicit_compiler.empty()) {
    return CompilerSelection{true, std::string(explicit_compiler),
                             CompilerConfiguration::explicit_option, {}};
  }
  if (!environment_compiler.empty()) {
    return CompilerSelection{true, std::string(environment_compiler),
                             CompilerConfiguration::environment, {}};
  }
  const std::string fallback =
      platform == NativePlatform::windows_msvc ? "cl.exe" : "cc";
  if (!executable_exists(fallback)) {
    return CompilerSelection{false, {}, CompilerConfiguration::fallback,
                             "no C compiler found by platform fallback ('" +
                                 fallback + "')"};
  }
  return CompilerSelection{true, fallback, CompilerConfiguration::fallback, {}};
}

std::vector<std::string>
make_c_compiler_arguments(NativePlatform platform,
                          std::string_view compiler,
                          std::string_view c_source_path,
                          std::string_view native_output_path) {
  const NativePlatform style = compiler_platform(platform, compiler);
  if (style == NativePlatform::windows_msvc) {
    return {"/nologo", "/std:c11", std::string(c_source_path),
            "/Fe:" + std::string(native_output_path)};
  }
  return {"-std=c11", std::string(c_source_path), "-o",
          std::string(native_output_path)};
}

NativeBuildResult build_native(const NativeBuildRequest &request) {
  CompilerSelection selection =
      select_c_compiler(request.explicit_compiler, request.environment_compiler,
                        native_platform());
  if (!selection.ok) {
    return NativeBuildResult{false, selection.error};
  }

  std::error_code error;
  std::string launch_executable = selection.executable;
  const std::filesystem::path selected_path(selection.executable);
  if (has_path_separator(selection.executable) && selected_path.is_relative()) {
    const std::filesystem::path absolute_selection =
        std::filesystem::absolute(selected_path, error);
    if (error) {
      return NativeBuildResult{
          false, "compiler selected from " +
                     std::string(compiler_configuration_name(
                         selection.configuration)) +
                     " ('" + selection.executable +
                     "') has an invalid executable path"};
    }
    selection.executable = path_string(absolute_selection);
    launch_executable = selection.executable;
  }
#ifndef _WIN32
  if (!has_path_separator(selection.executable)) {
    const std::string resolved = resolve_path_executable(selection.executable);
    if (!resolved.empty()) {
      launch_executable = resolved;
    }
  }
#endif

  error.clear();
  std::filesystem::path output =
      std::filesystem::absolute(std::filesystem::path(std::string(request.output_path)),
                                error);
  if (error || output.filename().empty()) {
    return NativeBuildResult{false, "unable to prepare native output path"};
  }
  const std::filesystem::path parent = output.parent_path();
  std::filesystem::path temporary_directory;
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    std::string suffix = ".bennu-build.tmp";
    if (attempt != 0) {
      suffix += '.' + std::to_string(attempt);
    }
    temporary_directory = parent / (output.filename().string() + suffix);
    error.clear();
    if (std::filesystem::create_directory(temporary_directory, error)) {
      break;
    }
    if (error && error != std::errc::file_exists) {
      return NativeBuildResult{false, "unable to create isolated build temporary directory"};
    }
    temporary_directory.clear();
  }
  if (temporary_directory.empty()) {
    return NativeBuildResult{false, "unable to create isolated build temporary directory"};
  }

  const std::filesystem::path c_source = temporary_directory / "program.c";
  const std::filesystem::path staging_output =
      temporary_directory /
      (native_platform() == NativePlatform::windows_msvc ? "program.exe" : "program");
  const std::filesystem::path compiler_log = temporary_directory / "compiler.log";
  if (!write_bytes(c_source, request.c_source)) {
    return failure_with_cleanup(temporary_directory,
                                "unable to write temporary C source");
  }

  const std::vector<std::string> arguments = make_c_compiler_arguments(
      native_platform(), selection.executable, path_string(c_source),
      path_string(staging_output));
  const ProcessResult process = run_process(launch_executable, arguments,
                                            temporary_directory, compiler_log);
  if (!process.started || process.terminated || process.exit_code != 0) {
    const std::string failure = process_failure(selection, process);
    return failure_with_cleanup(temporary_directory, failure);
  }
  error.clear();
  if (!is_usable_native_output(staging_output, error)) {
    return failure_with_cleanup(
        temporary_directory,
        "compiler selected from " +
            std::string(compiler_configuration_name(selection.configuration)) +
            " ('" + selection.executable +
            "') reported success without producing usable native output");
  }

  std::filesystem::path replacement;
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    std::string suffix = ".bennu-native.tmp";
    if (attempt != 0) {
      suffix += '.' + std::to_string(attempt);
    }
    const std::filesystem::path candidate = parent / (output.filename().string() + suffix);
    if (reserve_replacement_path(candidate)) {
      if (replace_output(staging_output, candidate)) {
        replacement = candidate;
        break;
      }
      error.clear();
      std::filesystem::remove(candidate, error);
    }
  }
  if (replacement.empty()) {
    return failure_with_cleanup(temporary_directory,
                                "unable to stage compiled native output");
  }

  error.clear();
  std::filesystem::remove_all(temporary_directory, error);
  if (error) {
    std::error_code removal_error;
    std::filesystem::remove(replacement, removal_error);
    std::string message = "unable to clean isolated build temporary directory";
    if (removal_error) {
      message += "; unable to remove staged native output";
    }
    return NativeBuildResult{false, std::move(message)};
  }
  if (!replace_output(replacement, output)) {
    error.clear();
    std::filesystem::remove(replacement, error);
    std::string message = "unable to replace native output";
    if (error) {
      message += "; unable to remove staged native output";
    }
    return NativeBuildResult{false, std::move(message)};
  }
  return NativeBuildResult{true, {}};
}

} // namespace bennu
