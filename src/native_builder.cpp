#include "bennu/native_builder.hpp"
#include "bennu/path_encoding.hpp"

#include "doctest/doctest.h"

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

bool write_bytes(const std::filesystem::path &path, std::string_view bytes) {
#ifdef _WIN32
  std::FILE *file = _wfopen(path.c_str(), L"wb");
#else
  std::FILE *file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool write_failed =
      std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size();
  const bool close_failed = std::fclose(file) != 0;
  return !write_failed && !close_failed;
}

std::string read_process_output(const std::filesystem::path &path) {
#ifdef _WIN32
  std::FILE *file = _wfopen(path.c_str(), L"rb");
#else
  std::FILE *file = std::fopen(path.c_str(), "rb");
#endif
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

PathToUtf8Result
path_to_ordinary_utf8(const std::filesystem::path &path) {
  PathToUtf8Result result = path_to_utf8(path);
#ifdef _WIN32
  if (!result.ok) {
    return result;
  }
  constexpr std::string_view extended_unc_prefix = "\\\\?\\UNC\\";
  constexpr std::string_view extended_prefix = "\\\\?\\";
  if (result.text.starts_with(extended_unc_prefix)) {
    result.text = "\\\\" + result.text.substr(extended_unc_prefix.size());
  } else if (result.text.starts_with(extended_prefix)) {
    result.text.erase(0, extended_prefix.size());
  }
#endif
  return result;
}

struct CompilerWorkspaceResult {
  bool ok;
  std::filesystem::path path;
};

#ifdef _WIN32
CompilerWorkspaceResult create_compiler_workspace() {
  std::error_code error;
  std::filesystem::path root = std::filesystem::temp_directory_path(error);
  if (error) {
    return CompilerWorkspaceResult{false, {}};
  }

  constexpr std::size_t workspace_name_reserve = 64;
  if (root.native().size() + workspace_name_reserve >= MAX_PATH) {
    std::wstring short_root(MAX_PATH, L'\0');
    const DWORD size = GetShortPathNameW(root.c_str(), short_root.data(),
                                         static_cast<DWORD>(short_root.size()));
    if (size == 0 || static_cast<std::size_t>(size) >= short_root.size()) {
      return CompilerWorkspaceResult{false, {}};
    }
    short_root.resize(size);
    root = std::filesystem::path(std::move(short_root));
  }

  const std::wstring stem =
      L"bennu-build-" + std::to_wstring(GetCurrentProcessId()) + L'-' +
      std::to_wstring(GetTickCount64()) + L'-';
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    const std::filesystem::path candidate =
        root / (stem + std::to_wstring(attempt));
    error.clear();
    if (std::filesystem::create_directory(candidate, error)) {
      return CompilerWorkspaceResult{true, candidate};
    }
    if (error && error != std::errc::file_exists) {
      return CompilerWorkspaceResult{false, {}};
    }
  }
  return CompilerWorkspaceResult{false, {}};
}
#endif

#ifdef _WIN32
bool executable_exists(std::string_view executable) {
  const WideStringResult name = utf8_to_wide(executable);
  if (!name.ok) {
    return false;
  }
  if (has_path_separator(executable) || executable.find(':') != std::string_view::npos) {
    const DWORD attributes = GetFileAttributesW(name.text.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  }
  const DWORD required =
      SearchPathW(nullptr, name.text.c_str(), nullptr, 0, nullptr, nullptr);
  return required != 0;
}

std::wstring quote_windows_argument(std::wstring_view argument) {
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted += L'"';
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted += character;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted += L'"';
  return quoted;
}

ProcessResult run_process(std::string_view executable,
                          const std::vector<std::string> &arguments,
                          const std::filesystem::path &working_directory,
                          const std::filesystem::path &log_path) {
  HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE,
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

  const WideStringResult wide_executable = utf8_to_wide(executable);
  if (!wide_executable.ok) {
    CloseHandle(log);
    return ProcessResult{false, false, 0,
                         static_cast<int>(ERROR_NO_UNICODE_TRANSLATION), {}};
  }
  std::wstring command_line = quote_windows_argument(wide_executable.text);
  for (const std::string &argument : arguments) {
    const WideStringResult wide_argument = utf8_to_wide(argument);
    if (!wide_argument.ok) {
      CloseHandle(log);
      return ProcessResult{false, false, 0,
                           static_cast<int>(ERROR_NO_UNICODE_TRANSLATION), {}};
    }
    command_line += L' ';
    command_line += quote_windows_argument(wide_argument.text);
  }
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log;
  startup.hStdError = log;
  PROCESS_INFORMATION process{};
  const wchar_t *application_name =
      has_path_separator(executable) ||
              executable.find(':') != std::string_view::npos
          ? wide_executable.text.c_str()
          : nullptr;
  const PathToUtf8Result ordinary_working_directory =
      path_to_ordinary_utf8(working_directory);
  const WideStringResult wide_working_directory =
      ordinary_working_directory.ok
          ? utf8_to_wide(ordinary_working_directory.text)
          : WideStringResult{false, {}};
  if (!wide_working_directory.ok) {
    CloseHandle(log);
    return ProcessResult{false, false, 0,
                         static_cast<int>(ERROR_NO_UNICODE_TRANSLATION), {}};
  }
  const BOOL created = CreateProcessW(
      application_name, mutable_command.data(), nullptr, nullptr, TRUE, 0,
      nullptr, wide_working_directory.text.c_str(), &startup, &process);
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
        return absolute.native();
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
  const int log =
      open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
    if (chdir(working_directory.c_str()) != 0 || dup2(log, STDOUT_FILENO) < 0 ||
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
  const std::size_t separator = compiler.find_last_of("/\\");
  std::string name(compiler.substr(
      separator == std::string_view::npos ? 0 : separator + 1));
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

PathToUtf8Result
path_to_compiler_argument(const std::filesystem::path &path,
                          NativePlatform style) {
  return style == NativePlatform::windows_msvc ? path_to_ordinary_utf8(path)
                                                : path_to_utf8(path);
}

bool replace_output(const std::filesystem::path &temporary,
                    const std::filesystem::path &output) {
#ifdef _WIN32
  return MoveFileExW(temporary.c_str(), output.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return std::rename(temporary.c_str(), output.c_str()) == 0;
#endif
}

bool reserve_replacement_path(const std::filesystem::path &path) {
#ifdef _WIN32
  const int descriptor = _wopen(path.c_str(),
                                _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                                _S_IREAD | _S_IWRITE);
  if (descriptor < 0) {
    return false;
  }
  if (_close(descriptor) != 0) {
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    return false;
  }
#else
  const int descriptor =
      open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    return false;
  }
  if (close(descriptor) != 0) {
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    return false;
  }
#endif
  return true;
}

NativeBuildResult
failure_with_cleanup(const std::filesystem::path &temporary,
                     const std::filesystem::path &compiler_workspace,
                     std::string message) {
  std::error_code cleanup_error;
  if (compiler_workspace != temporary) {
    std::filesystem::remove_all(compiler_workspace, cleanup_error);
  }
  std::error_code temporary_cleanup_error;
  std::filesystem::remove_all(temporary, temporary_cleanup_error);
  if (cleanup_error || temporary_cleanup_error) {
    message += "; unable to clean isolated build temporary directory";
  }
  return NativeBuildResult{false, std::move(message)};
}

std::string output_path_failure(std::string_view message,
                                std::string_view output_path) {
  std::string error(message);
  error += " for '";
  error += output_path;
  error += '\'';
  return error;
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
  return access(path.c_str(), X_OK) == 0;
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
    return {"/nologo", "/std:c11", "/fp:strict", std::string(c_source_path),
            "/Fe:" + std::string(native_output_path),
            "/Fo:" + std::string(native_output_path) + ".obj"};
  }
  return {"-std=c11",       "-frounding-math", "-ffp-contract=off",
          "-fno-fast-math", std::string(c_source_path),
          "-o",             std::string(native_output_path), "-lm"};
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
  if (has_path_separator(selection.executable) ||
      selection.executable.find(':') != std::string_view::npos) {
    const PathFromUtf8Result selected_result =
        path_for_io_from_utf8(selection.executable);
    if (!selected_result.ok) {
      return NativeBuildResult{
          false, "compiler selected from " +
                     std::string(compiler_configuration_name(
                         selection.configuration)) +
                     " ('" + selection.executable +
                     "') has an invalid executable path"};
    }
    std::filesystem::path launch_path = selected_result.path;
    if (launch_path.is_relative()) {
      launch_path = std::filesystem::absolute(launch_path, error);
    }
    PathToUtf8Result launch_text = path_to_utf8(launch_path);
    if (error || !launch_text.ok) {
      return NativeBuildResult{
          false, "compiler selected from " +
                     std::string(compiler_configuration_name(
                         selection.configuration)) +
                     " ('" + selection.executable +
                     "') has an invalid executable path"};
    }
    launch_executable = std::move(launch_text.text);
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
  const PathFromUtf8Result output_result =
      path_for_io_from_utf8(request.output_path);
  if (!output_result.ok) {
    return NativeBuildResult{
        false,
        output_path_failure("unable to prepare native output path",
                            request.output_path)};
  }
  std::filesystem::path output =
      std::filesystem::absolute(output_result.path, error);
  if (error || output.filename().empty()) {
    return NativeBuildResult{
        false,
        output_path_failure("unable to prepare native output path",
                            request.output_path)};
  }
  const std::filesystem::path parent = output.parent_path();
  std::filesystem::path temporary_directory;
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    std::string suffix = ".bennu-build.tmp";
    if (attempt != 0) {
      suffix += '.' + std::to_string(attempt);
    }
    std::filesystem::path temporary_name = output.filename();
    temporary_name += suffix;
    temporary_directory = parent / temporary_name;
    error.clear();
    if (std::filesystem::create_directory(temporary_directory, error)) {
      break;
    }
    if (error && error != std::errc::file_exists) {
      return NativeBuildResult{
          false,
          output_path_failure(
              "unable to create isolated build temporary directory",
              request.output_path)};
    }
    temporary_directory.clear();
  }
  if (temporary_directory.empty()) {
    return NativeBuildResult{
        false,
        output_path_failure("unable to create isolated build temporary directory",
                            request.output_path)};
  }

  std::filesystem::path compiler_workspace = temporary_directory;
#ifdef _WIN32
  const CompilerWorkspaceResult workspace_result = create_compiler_workspace();
  if (!workspace_result.ok) {
    return failure_with_cleanup(
        temporary_directory, temporary_directory,
        output_path_failure("unable to create compiler temporary directory",
                            request.output_path));
  }
  compiler_workspace = workspace_result.path;
#endif

  const std::filesystem::path c_source = compiler_workspace / "program.c";
  const std::filesystem::path staging_output =
      temporary_directory /
      (native_platform() == NativePlatform::windows_msvc ? "program.exe" : "program");
  const std::filesystem::path compiler_output =
      compiler_workspace /
      (native_platform() == NativePlatform::windows_msvc ? "program.exe" : "program");
  const std::filesystem::path compiler_log = temporary_directory / "compiler.log";
  if (!write_bytes(c_source, request.c_source)) {
    return failure_with_cleanup(
        temporary_directory, compiler_workspace,
        output_path_failure("unable to write temporary C source",
                            request.output_path));
  }

  const NativePlatform compiler_style =
      compiler_platform(native_platform(), selection.executable);
  const std::filesystem::path compiler_source =
      compiler_style == NativePlatform::windows_msvc ? c_source.filename()
                                                      : c_source;
  const std::filesystem::path compiler_output_argument =
      compiler_style == NativePlatform::windows_msvc ? compiler_output.filename()
                                                      : compiler_output;
  PathToUtf8Result c_source_text =
      path_to_compiler_argument(compiler_source, compiler_style);
  PathToUtf8Result staging_output_text =
      path_to_compiler_argument(compiler_output_argument, compiler_style);
  if (!c_source_text.ok || !staging_output_text.ok) {
    return failure_with_cleanup(temporary_directory, compiler_workspace,
                                "unable to encode compiler paths as UTF-8");
  }
  const std::vector<std::string> arguments = make_c_compiler_arguments(
      native_platform(), selection.executable, c_source_text.text,
      staging_output_text.text);
  const ProcessResult process = run_process(launch_executable, arguments,
                                            compiler_workspace, compiler_log);
  if (!process.started || process.terminated || process.exit_code != 0) {
    const std::string failure = process_failure(selection, process);
    return failure_with_cleanup(temporary_directory, compiler_workspace,
                                failure);
  }
  error.clear();
  if (!is_usable_native_output(compiler_output, error)) {
    return failure_with_cleanup(
        temporary_directory, compiler_workspace,
        "compiler selected from " +
            std::string(compiler_configuration_name(selection.configuration)) +
            " ('" + selection.executable +
            "') reported success without producing usable native output");
  }

  if (compiler_output != staging_output) {
    error.clear();
    std::filesystem::copy_file(compiler_output, staging_output,
                               std::filesystem::copy_options::none, error);
    if (error) {
      return failure_with_cleanup(
          temporary_directory, compiler_workspace,
          output_path_failure("unable to stage compiled native output",
                              request.output_path));
    }
  }

  std::filesystem::path replacement;
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    std::string suffix = ".bennu-native.tmp";
    if (attempt != 0) {
      suffix += '.' + std::to_string(attempt);
    }
    std::filesystem::path candidate_name = output.filename();
    candidate_name += suffix;
    const std::filesystem::path candidate = parent / candidate_name;
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
    return failure_with_cleanup(
        temporary_directory, compiler_workspace,
        output_path_failure("unable to stage compiled native output",
                            request.output_path));
  }

  error.clear();
  if (compiler_workspace != temporary_directory) {
    std::filesystem::remove_all(compiler_workspace, error);
  }
  std::error_code temporary_cleanup_error;
  std::filesystem::remove_all(temporary_directory, temporary_cleanup_error);
  if (error || temporary_cleanup_error) {
    std::error_code removal_error;
    std::filesystem::remove(replacement, removal_error);
    std::string message = output_path_failure(
        "unable to clean isolated build temporary directory",
        request.output_path);
    if (removal_error) {
      message += "; unable to remove staged native output";
    }
    return NativeBuildResult{false, std::move(message)};
  }
  if (!replace_output(replacement, output)) {
    error.clear();
    std::filesystem::remove(replacement, error);
    std::string message = output_path_failure(
        "unable to replace native output", request.output_path);
    if (error) {
      message += "; unable to remove staged native output";
    }
    return NativeBuildResult{false, std::move(message)};
  }
  return NativeBuildResult{true, {}};
}

TEST_CASE("native compiler selection preserves precedence and attribution") {
  const CompilerSelection explicit_selection =
      select_c_compiler("explicit cc", "environment cc",
                        NativePlatform::gcc_like);
  REQUIRE(explicit_selection.ok);
  CHECK(explicit_selection.executable == "explicit cc");
  CHECK(explicit_selection.configuration ==
        CompilerConfiguration::explicit_option);

  const CompilerSelection environment_selection =
      select_c_compiler("", "environment cc", NativePlatform::gcc_like);
  REQUIRE(environment_selection.ok);
  CHECK(environment_selection.executable == "environment cc");
  CHECK(environment_selection.configuration ==
        CompilerConfiguration::environment);
}

TEST_CASE("native compiler command lines preserve argument boundaries") {
  const std::vector<std::string> gcc = make_c_compiler_arguments(
      NativePlatform::gcc_like, "clang", "/tmp/source ; $.c",
      "/tmp/output ; $");
  CHECK(gcc == std::vector<std::string>{
                   "-std=c11", "-frounding-math", "-ffp-contract=off",
                   "-fno-fast-math", "/tmp/source ; $.c", "-o",
                   "/tmp/output ; $", "-lm"});

  const std::vector<std::string> msvc = make_c_compiler_arguments(
      NativePlatform::windows_msvc, "cl.exe", "C:\\source ; $.c",
      "C:\\output ; $.exe");
  CHECK(msvc == std::vector<std::string>{"/nologo", "/std:c11", "/fp:strict",
                                         "C:\\source ; $.c",
                                         "/Fe:C:\\output ; $.exe",
                                         "/Fo:C:\\output ; $.exe.obj"});
}

} // namespace bennu
