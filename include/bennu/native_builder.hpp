#ifndef BENNU_NATIVE_BUILDER_HPP
#define BENNU_NATIVE_BUILDER_HPP

#include <string>
#include <string_view>
#include <vector>

namespace bennu {

enum class NativePlatform {
  gcc_like,
  windows_msvc,
};

enum class CompilerConfiguration {
  explicit_option,
  environment,
  fallback,
};

struct CompilerSelection {
  bool ok;
  std::string executable;
  CompilerConfiguration configuration;
  std::string error;
};

struct NativeBuildRequest {
  std::string_view c_source;
  std::string_view output_path;
  std::string_view explicit_compiler;
  std::string_view environment_compiler;
};

struct NativeBuildResult {
  bool ok;
  std::string error;
};

NativePlatform native_platform();
CompilerSelection select_c_compiler(std::string_view explicit_compiler,
                                    std::string_view environment_compiler,
                                    NativePlatform platform);
std::vector<std::string>
make_c_compiler_arguments(NativePlatform platform,
                          std::string_view compiler,
                          std::string_view c_source_path,
                          std::string_view native_output_path);
NativeBuildResult build_native(const NativeBuildRequest &request);
std::string_view compiler_configuration_name(CompilerConfiguration configuration);

} // namespace bennu

#endif
