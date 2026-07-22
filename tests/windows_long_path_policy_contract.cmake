if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

set(manifest "${BENNU_SOURCE_DIR}/src/bennu.exe.manifest")
if(NOT EXISTS "${manifest}")
  message(FATAL_ERROR "Windows long-path-aware application manifest is missing")
endif()

file(READ "${manifest}" manifest_text)
file(READ "${BENNU_SOURCE_DIR}/src/bennu-version.rc.in" resource_text)
file(READ "${BENNU_SOURCE_DIR}/CMakeLists.txt" cmake_text)
file(READ "${BENNU_SOURCE_DIR}/include/bennu/path_encoding.hpp" header_text)
file(READ "${BENNU_SOURCE_DIR}/src/path_encoding.cpp" implementation_text)
file(READ "${BENNU_SOURCE_DIR}/src/native_builder.cpp" native_builder_text)
file(READ "${BENNU_SOURCE_DIR}/tests/windows_long_paths.cmake" native_test_text)
file(READ "${BENNU_SOURCE_DIR}/README.md" readme_text)
file(READ "${BENNU_SOURCE_DIR}/doc/decision-diary.md" diary_text)

function(require_text variable expected description)
  string(FIND "${${variable}}" "${expected}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "missing ${description}: ${expected}")
  endif()
endfunction()

require_text(manifest_text
  "<longPathAware xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">true</longPathAware>"
  "longPathAware application declaration")
require_text(manifest_text
  "<requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\" />"
  "asInvoker execution policy")
require_text(resource_text "RT_MANIFEST" "embedded PE manifest resource")
require_text(resource_text "@BENNU_MANIFEST_RESOURCE_PATH@"
  "configured manifest resource input")
require_text(cmake_text "BENNU_MANIFEST_RESOURCE_PATH"
  "configured manifest resource path")
require_text(cmake_text "src/bennu.exe.manifest" "manifest resource source")
require_text(header_text "path_for_io_from_utf8"
  "explicit UTF-8-to-I/O-path boundary")
require_text(implementation_text "UNC" "extended-length UNC normalization")
require_text(implementation_text "native.substr(2)"
  "UNC server/share preservation")
require_text(implementation_text "path.has_root_name()"
  "extended-length drive-path normalization")
require_text(implementation_text "path.make_preferred()"
  "extended-length separator normalization")
require_text(native_builder_text "path_to_compiler_argument"
  "ordinary MSVC path argument boundary")
require_text(native_builder_text "\"/Fo:\""
  "absolute MSVC object path without a process working-directory dependency")
require_text(native_test_text "ordinary space éß 文 🐍"
  "lossless target-native long-path corpus")
require_text(native_test_text "set(bennu"
  "extracted-release executable journey")
require_text(native_test_text "COMMAND \"\${real_native_output}\""
  "real long-path native output execution")
require_text(readme_text "callers do not add a `\\\\?\\` prefix"
  "ordinary Windows long-path user contract")
require_text(diary_text
  "### 2026-07-22 — Normalize Windows filesystem boundaries while preserving ordinary paths"
  "Issue #39 long-path policy decision")
