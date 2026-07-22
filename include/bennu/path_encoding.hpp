#ifndef BENNU_PATH_ENCODING_HPP
#define BENNU_PATH_ENCODING_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace bennu {

struct PathFromUtf8Result {
  bool ok;
  std::filesystem::path path;
};

struct PathToUtf8Result {
  bool ok;
  std::string text;
};

PathFromUtf8Result path_from_utf8(std::string_view text);
PathFromUtf8Result path_for_io_from_utf8(std::string_view text);
PathToUtf8Result path_to_utf8(const std::filesystem::path &path);

#ifdef _WIN32
struct WideStringResult {
  bool ok;
  std::wstring text;
};

struct Utf8StringResult {
  bool ok;
  std::string text;
};

WideStringResult utf8_to_wide(std::string_view text);
Utf8StringResult wide_to_utf8(std::wstring_view text);
#endif

} // namespace bennu

#endif
