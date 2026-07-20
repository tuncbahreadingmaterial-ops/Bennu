#include "rewrite_backend.hpp"

namespace bennu {

NativeBuildResult
build_rewrite_native_internal(const RewriteNativeBackendRequest &request) {
  const RewriteCBackendResult emitted =
      emit_rewrite_c_source_internal(request.source, request.configuration);
  if (!emitted.ok) {
    return NativeBuildResult{false, emitted.error};
  }
  return build_native(NativeBuildRequest{emitted.source, request.output_path,
                                         request.explicit_compiler,
                                         request.environment_compiler});
}

} // namespace bennu
