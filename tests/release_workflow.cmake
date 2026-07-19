set(release_workflow "${BENNU_SOURCE_DIR}/.github/workflows/release.yml")

if(NOT EXISTS "${release_workflow}")
  message(FATAL_ERROR "release workflow is missing: ${release_workflow}")
endif()

file(READ "${release_workflow}" workflow_text)
if(NOT workflow_text MATCHES "name: v0.1.0 Release")
  message(FATAL_ERROR "release workflow does not have the fixed v0.1.0 identity")
endif()

function(require_workflow_text expected description)
  string(FIND "${workflow_text}" "${expected}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "release workflow is missing ${description}: ${expected}")
  endif()
endfunction()

string(FIND "${workflow_text}" "pull_request" pull_request_at)
if(NOT pull_request_at EQUAL -1)
  message(FATAL_ERROR "release workflow must not add pull-request automation")
endif()

require_workflow_text("workflow_dispatch:" "the manual dry-run trigger")
require_workflow_text("force_failure:" "the controlled-failure input")
require_workflow_text("- v0.1.0" "the exact production tag trigger")
require_workflow_text("permissions:\n  contents: read" "read-only default permissions")
require_workflow_text("runner: ubuntu-24.04" "the Linux x64 runner")
require_workflow_text("runner: windows-2022" "the Windows x64 runner")
require_workflow_text("runner: macos-15" "the macOS arm64 runner")
require_workflow_text("bennu-v0.1.0-linux-x64.tar.gz" "the Linux archive name")
require_workflow_text("bennu-v0.1.0-windows-x64.zip" "the Windows archive name")
require_workflow_text("bennu-v0.1.0-macos-arm64.tar.gz" "the macOS archive name")
require_workflow_text("ctest --test-dir build --output-on-failure" "complete target-native CTest")
require_workflow_text("tools/release/package-and-smoke.ps1" "target-native package smoke script")
require_workflow_text("actions/upload-artifact@" "temporary artifact upload")
require_workflow_text("retention-days: 1" "short temporary artifact retention")

set(package_script "${BENNU_SOURCE_DIR}/tools/release/package-and-smoke.ps1")
if(NOT EXISTS "${package_script}")
  message(FATAL_ERROR "package-and-smoke script is missing: ${package_script}")
endif()
file(READ "${package_script}" package_text)
foreach(required_text IN ITEMS
    "emit-c"
    "Remove-Item Env:CC"
    "ZipFile"
    "tar"
    "GetRelativePath"
    "GetUnixFileMode"
    "ExtractToDirectory")
  string(FIND "${package_text}" "${required_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "package-and-smoke script is missing: ${required_text}")
  endif()
endforeach()

require_workflow_text("publish:\n    name: Verify and publish v0.1.0" "the publication job")
require_workflow_text("needs:\n      - package" "publication dependency on every matrix package job")
require_workflow_text("github.event_name == 'push' && github.ref == 'refs/tags/v0.1.0'" "exact tag-only publication condition")
require_workflow_text("contents: write" "publication write permission")
require_workflow_text("actions/download-artifact@" "verified archive download")
require_workflow_text("merge-multiple: true" "single publication staging directory")
require_workflow_text("tools/release/publish-release.sh" "draft/verify/publish implementation")

string(REGEX MATCHALL "contents: write" write_permissions "${workflow_text}")
list(LENGTH write_permissions write_permission_count)
if(NOT write_permission_count EQUAL 1)
  message(FATAL_ERROR "release workflow must grant contents: write exactly once, on publication")
endif()

set(publish_script "${BENNU_SOURCE_DIR}/tools/release/publish-release.sh")
if(NOT EXISTS "${publish_script}")
  message(FATAL_ERROR "publication script is missing: ${publish_script}")
endif()
file(READ "${publish_script}" publish_text)
foreach(required_text IN ITEMS
    "refs/tags/v0.1.0^{commit}"
    "bennu-v0.1.0-linux-x64.tar.gz"
    "bennu-v0.1.0-windows-x64.zip"
    "bennu-v0.1.0-macos-arm64.tar.gz"
    "draft=true"
    "prerelease=false"
    "cmp --silent"
    "--method PATCH"
    "release_by_id_endpoint="
    "--method PATCH \"$release_by_id_endpoint\""
    "draft=false"
    "existing release assets do not exactly match")
  string(FIND "${publish_text}" "${required_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "publication script is missing: ${required_text}")
  endif()
endforeach()

set(decision_diary "${BENNU_SOURCE_DIR}/doc/decision-diary.md")
file(READ "${decision_diary}" diary_text)
string(FIND "${diary_text}" "Gate v0.1.0 publication behind target-native verified archives" diary_entry_at)
if(diary_entry_at EQUAL -1)
  message(FATAL_ERROR "decision diary is missing the Issue #10 release topology decision")
endif()
