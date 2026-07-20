#include "rewrite_backend.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {

bool read_file(const char *path, std::string &bytes) {
  std::FILE *file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  char buffer[4096];
  while (true) {
    const std::size_t count = std::fread(buffer, 1U, sizeof(buffer), file);
    bytes.append(buffer, count);
    if (count != sizeof(buffer)) {
      const bool ok = std::feof(file) != 0 && std::ferror(file) == 0;
      return std::fclose(file) == 0 && ok;
    }
  }
}

bool write_file(const char *path, std::string_view bytes, const char *mode) {
  std::FILE *file = std::fopen(path, mode);
  if (file == nullptr) {
    return false;
  }
  const bool written =
      std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size();
  return std::fclose(file) == 0 && written;
}

bennu::RewriteBackendConfiguration configuration_for(std::string_view mode) {
  bennu::RewriteBackendConfiguration configuration{
      bennu::ExecutionProfile::trusted_local_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      bennu::AllocationFailureInjection{std::nullopt},
      bennu::AllocationFailureInjection{std::nullopt}};
  if (mode.find("bounded") != std::string_view::npos) {
    configuration.profile = bennu::ExecutionProfile::bounded_v1;
    configuration.limits = bennu::ResourceLimits{
        std::size_t{65536U}, std::size_t{262144U}, std::size_t{262144U}};
  }
  if (mode.find("bounded-fail") != std::string_view::npos) {
    configuration.limits.max_work_units = 0U;
  }
  if (mode.find("bounded-exact") != std::string_view::npos) {
    configuration.limits = bennu::ResourceLimits{
        std::size_t{8U}, std::size_t{16U}, std::size_t{2U}};
  }
  if (mode.find("bounded-live-fail") != std::string_view::npos) {
    configuration.limits = bennu::ResourceLimits{
        std::size_t{8U}, std::size_t{15U}, std::size_t{2U}};
  }
  if (mode.find("validation-fail") != std::string_view::npos) {
    configuration.validation_allocation_failure
        .fail_at_reservation_ordinal = 0U;
  }
  if (mode.find("validation-fail-second") != std::string_view::npos) {
    configuration.validation_allocation_failure
        .fail_at_reservation_ordinal = 1U;
  }
  if (mode.find("runtime-fail-first") != std::string_view::npos) {
    configuration.runtime_allocation_failure.fail_at_reservation_ordinal = 0U;
  }
  if (mode.find("runtime-fail-second") != std::string_view::npos) {
    configuration.runtime_allocation_failure.fail_at_reservation_ordinal = 1U;
  }
  return configuration;
}

} // namespace

int main(int argument_count, char **arguments) {
  if (argument_count != 4 && argument_count != 5) {
    return 2;
  }
  std::string source;
  if (!read_file(arguments[2], source)) {
    return 3;
  }
  const std::string_view mode(arguments[1]);
  const bennu::RewriteBackendConfiguration configuration =
      configuration_for(mode);
  if (mode.find("oracle") == 0U) {
    const bennu::RewriteEvaluationTextResult evaluated =
        bennu::evaluate_rewrite_text_internal(source, configuration);
    if (!evaluated.ok) {
      static_cast<void>(std::fwrite(evaluated.error.data(), 1U,
                                    evaluated.error.size(), stderr));
      return 4;
    }
    return write_file(arguments[3], evaluated.output, "w") ? 0 : 5;
  }
  if (mode.find("build") == 0U) {
    if (argument_count != 5) {
      return 2;
    }
    const bennu::NativeBuildResult built = bennu::build_rewrite_native_internal(
        bennu::RewriteNativeBackendRequest{source, configuration, arguments[3],
                                           arguments[4], ""});
    if (!built.ok) {
      static_cast<void>(
          std::fwrite(built.error.data(), 1U, built.error.size(), stderr));
      return 6;
    }
    return 0;
  }
  if (mode.find("emit") != 0U || argument_count != 4) {
    return 2;
  }
  const bennu::RewriteCBackendResult emitted =
      bennu::emit_rewrite_c_source_internal(source, configuration);
  if (!emitted.ok) {
    static_cast<void>(
        std::fwrite(emitted.error.data(), 1U, emitted.error.size(), stderr));
    return 4;
  }
  return write_file(arguments[3], emitted.source, "wb") ? 0 : 5;
}
