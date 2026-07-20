#ifndef BENNU_REWRITE_BACKEND_HPP
#define BENNU_REWRITE_BACKEND_HPP

#include "bennu/native_builder.hpp"
#include "bennu/resources.hpp"

#include <string>
#include <string_view>

namespace bennu {

struct RewriteBackendConfiguration {
  ExecutionProfile profile;
  ResourceLimits limits;
  AllocationFailureInjection validation_allocation_failure;
  AllocationFailureInjection runtime_allocation_failure;
};

struct RewriteCBackendResult {
  bool ok;
  std::string source;
  std::string error;
};

#ifdef BENNU_REWRITE_BACKEND_TEST_DRIVER
struct RewriteEvaluationTextResult {
  bool ok;
  std::string output;
  std::string error;
};
#endif

struct RewriteNativeBackendRequest {
  std::string_view source;
  RewriteBackendConfiguration configuration;
  std::string_view output_path;
  std::string_view explicit_compiler;
  std::string_view environment_compiler;
};

RewriteCBackendResult emit_rewrite_c_source_internal(
    std::string_view source, const RewriteBackendConfiguration &configuration);
#ifdef BENNU_REWRITE_BACKEND_TEST_DRIVER
RewriteEvaluationTextResult evaluate_rewrite_text_internal(
    std::string_view source, const RewriteBackendConfiguration &configuration);
#endif
NativeBuildResult
build_rewrite_native_internal(const RewriteNativeBackendRequest &request);

} // namespace bennu

#endif
