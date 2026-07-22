#include "bennu/path_encoding.hpp"

#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bennu {

#ifdef _WIN32
WideStringResult utf8_to_wide(std::string_view text) {
  if (text.empty()) {
    return WideStringResult{true, {}};
  }
  if (text.size() >
      static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return WideStringResult{false, {}};
  }
  const int input_size = static_cast<int>(text.size());
  const int output_size = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, nullptr, 0);
  if (output_size == 0) {
    return WideStringResult{false, {}};
  }
  std::wstring output(static_cast<std::size_t>(output_size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size,
                          output.data(), output_size) != output_size) {
    return WideStringResult{false, {}};
  }
  return WideStringResult{true, std::move(output)};
}

Utf8StringResult wide_to_utf8(std::wstring_view text) {
  if (text.empty()) {
    return Utf8StringResult{true, {}};
  }
  if (text.size() >
      static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return Utf8StringResult{false, {}};
  }
  const int input_size = static_cast<int>(text.size());
  const int output_size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), input_size, nullptr, 0, nullptr,
      nullptr);
  if (output_size == 0) {
    return Utf8StringResult{false, {}};
  }
  std::string output(static_cast<std::size_t>(output_size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), input_size,
                          output.data(), output_size, nullptr,
                          nullptr) != output_size) {
    return Utf8StringResult{false, {}};
  }
  return Utf8StringResult{true, std::move(output)};
}
#endif

PathFromUtf8Result path_from_utf8(std::string_view text) {
#ifdef _WIN32
  WideStringResult converted = utf8_to_wide(text);
  if (!converted.ok) {
    return PathFromUtf8Result{false, {}};
  }
  return PathFromUtf8Result{true, std::filesystem::path(std::move(converted.text))};
#else
  return PathFromUtf8Result{true, std::filesystem::path(std::string(text))};
#endif
}

PathFromUtf8Result path_for_io_from_utf8(std::string_view text) {
  PathFromUtf8Result converted = path_from_utf8(text);
  if (!converted.ok) {
    return converted;
  }
#ifdef _WIN32
  std::filesystem::path path = std::move(converted.path);
  path.make_preferred();
  std::wstring native = path.native();
  // Keep diagnostics in the caller's UTF-8 spelling while Win32 I/O receives
  // one absolute extended-length path that does not depend on MAX_PATH.
  if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\")) {
    return PathFromUtf8Result{true, std::move(path)};
  }

  if (!path.is_absolute()) {
    std::error_code error;
    path = std::filesystem::absolute(path, error);
    if (error) {
      return PathFromUtf8Result{false, {}};
    }
  }
  path = path.lexically_normal();
  path.make_preferred();
  native = path.native();

  if (native.starts_with(L"\\\\")) {
    native = L"\\\\?\\UNC\\" + native.substr(2);
  } else if (path.has_root_name() && path.has_root_directory()) {
    native = L"\\\\?\\" + native;
  } else {
    return PathFromUtf8Result{false, {}};
  }
  return PathFromUtf8Result{true,
                            std::filesystem::path(std::move(native))};
#else
  return converted;
#endif
}

PathToUtf8Result path_to_utf8(const std::filesystem::path &path) {
#ifdef _WIN32
  Utf8StringResult converted = wide_to_utf8(path.native());
  return PathToUtf8Result{converted.ok, std::move(converted.text)};
#else
  return PathToUtf8Result{true, path.native()};
#endif
}

} // namespace bennu
