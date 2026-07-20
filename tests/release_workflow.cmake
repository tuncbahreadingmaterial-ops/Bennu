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
string(FIND "${workflow_text}" "| head -1" pipefail_head_at)
if(NOT pipefail_head_at EQUAL -1)
  message(FATAL_ERROR
    "release workflow must consume complete version output under pipefail")
endif()

require_workflow_text("workflow_dispatch:" "the manual dry-run trigger")
require_workflow_text("force_failure:" "the controlled-failure input")
require_workflow_text("- v0.1.0" "the exact production tag trigger")
require_workflow_text("permissions:\n  contents: read" "read-only default permissions")
require_workflow_text("runs-on: ubuntu-24.04" "the Linux x64 runner")
require_workflow_text("runner: windows-2022" "the Windows x64 runner")
require_workflow_text("runner: macos-15" "the macOS arm64 runner")
require_workflow_text("ubuntu:noble-20240423@sha256:562456a05a0dbd62a671c1854868862a4687bf979a96d48ae8e766642cd911e8" "the pinned Ubuntu 24.04 build/runtime image")
require_workflow_text("https://snapshot.ubuntu.com/ubuntu/20240424T000000Z/" "the pinned Ubuntu package snapshot")
require_workflow_text("DEBIAN_FRONTEND=noninteractive" "non-interactive pinned package installation")
require_workflow_text("useradd --create-home --uid 1001 bennu-ci" "unprivileged Linux test account")
require_workflow_text("runuser -u bennu-ci -- ctest --test-dir build --output-on-failure" "unprivileged complete Linux CTest")
require_workflow_text("runuser -u bennu-ci -- cmake --build build --verbose" "recorded Linux compile/link command lines")
require_workflow_text("git config --global --add safe.directory \"$GITHUB_WORKSPACE\"" "container checkout ownership trust before provenance inspection")
string(REGEX MATCHALL "libc6-dev=2\\.39-0ubuntu8" linux_libc_development_packages "${workflow_text}")
list(LENGTH linux_libc_development_packages linux_libc_development_package_count)
if(NOT linux_libc_development_package_count EQUAL 2)
  message(FATAL_ERROR
    "the pinned build and floor runtime jobs must both install libc6-dev explicitly")
endif()
require_workflow_text("bennu-v0.1.0-linux-x64.tar.gz" "the Linux archive name")
require_workflow_text("bennu-v0.1.0-windows-x64.zip" "the Windows archive name")
require_workflow_text("bennu-v0.1.0-macos-arm64.tar.gz" "the macOS archive name")
require_workflow_text("ctest --test-dir build --output-on-failure" "complete target-native CTest")
require_workflow_text("tools/release/package-and-smoke.ps1" "target-native package smoke script")
require_workflow_text("tools/release/package-linux.sh" "Linux package script")
require_workflow_text("tools/release/verify-linux-package.sh" "extracted Linux package journeys")
require_workflow_text("Verify Linux package on compatibility floor" "oldest-supported Linux execution job")
require_workflow_text("Verify Linux package on current userland" "current Linux execution job")
require_workflow_text("Verify durable Linux release asset" "durable Linux asset verification")
require_workflow_text("https://github.com/tuncbahreadingmaterial-ops/Bennu/releases/download/v0.1.0/bennu-v0.1.0-linux-x64.tar.gz" "durable Linux asset URL")
require_workflow_text("21f4608d1e32f83dcaf6ad121c2bac51181077330fcd8e5af31388e0504b85e6" "durable Linux asset SHA256")
require_workflow_text("Verify durable Windows release asset" "durable Windows asset verification")
require_workflow_text("https://github.com/tuncbahreadingmaterial-ops/Bennu/releases/download/v0.1.0/bennu-v0.1.0-windows-x64.zip" "durable Windows asset URL")
require_workflow_text("4b87f4af18251a6c4f19e60eb015421141bbe235354102b7f5ebd094035ebc12" "durable Windows asset SHA256")
require_workflow_text("Get-FileHash -Algorithm SHA256" "durable Windows asset hash verification")
require_workflow_text("actions/upload-artifact@" "temporary artifact upload")
require_workflow_text("retention-days: 1" "short temporary artifact retention")

set(cmake_configuration "${BENNU_SOURCE_DIR}/CMakeLists.txt")
file(READ "${cmake_configuration}" cmake_text)
foreach(required_cmake_text IN ITEMS
    "CMAKE_MSVC_RUNTIME_LIBRARY"
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  string(FIND "${cmake_text}" "${required_cmake_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR
      "CMake configuration is missing the dynamic MSVC runtime policy: ${required_cmake_text}")
  endif()
endforeach()

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
    "ExtractToDirectory"
    "dumpbin.exe"
    "/DEPENDENTS"
    "Assert-WindowsRuntimeDependencies"
    "MSVCP140.dll"
    "VCRUNTIME140.dll"
    "VCRUNTIME140_1.dll"
    "verify-clean-windows-package.ps1")
  string(FIND "${package_text}" "${required_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "package-and-smoke script is missing: ${required_text}")
  endif()
endforeach()

foreach(linux_release_script IN ITEMS
    "${BENNU_SOURCE_DIR}/tools/release/package-linux.sh"
    "${BENNU_SOURCE_DIR}/tools/release/verify-linux-package.sh"
    "${BENNU_SOURCE_DIR}/tools/release/verify-linux-elf.py")
  if(NOT EXISTS "${linux_release_script}")
    message(FATAL_ERROR "Linux compatibility script is missing: ${linux_release_script}")
  endif()
endforeach()

file(READ "${BENNU_SOURCE_DIR}/tools/release/verify-linux-elf.py" linux_elf_text)
foreach(required_linux_elf_text IN ITEMS
    "GLIBC_2.34"
    "GLIBCXX_3.4.32"
    "libstdc++.so.6"
    "libgcc_s.so.1"
    "libc.so.6"
    "libm.so.6"
    "readelf"
    "ldd")
  string(FIND "${linux_elf_text}" "${required_linux_elf_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR
      "Linux ELF verifier is missing policy text: ${required_linux_elf_text}")
  endif()
endforeach()

set(clean_windows_script
    "${BENNU_SOURCE_DIR}/tools/release/verify-clean-windows-package.ps1")
if(NOT EXISTS "${clean_windows_script}")
  message(FATAL_ERROR
    "clean Windows package smoke script is missing: ${clean_windows_script}")
endif()
file(READ "${clean_windows_script}" clean_windows_text)
foreach(required_clean_windows_text IN ITEMS
    "--help"
    "repl"
    "run"
    "emit-c"
    "build"
    "Remove-Item Env:CC"
    "no C compiler found by platform fallback"
    "System32"
    "HKLM:\\SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64"
    "14.51.36231.0")
  string(FIND "${clean_windows_text}" "${required_clean_windows_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR
      "clean Windows package smoke script is missing: ${required_clean_windows_text}")
  endif()
endforeach()

foreach(document IN ITEMS "${BENNU_SOURCE_DIR}/README.md"
                          "${BENNU_SOURCE_DIR}/doc/level1.md")
  file(READ "${document}" document_text)
  string(FIND "${document_text}" "Ubuntu 24.04 LTS x64 or newer" unsupported_linux_newer_at)
  if(NOT unsupported_linux_newer_at EQUAL -1)
    message(FATAL_ERROR
      "release deployment documentation promises an untested newer Ubuntu userland in ${document}")
  endif()
  foreach(required_document_text IN ITEMS
      "Ubuntu 24.04 LTS x64"
      "glibc 2.39 or newer"
      "GLIBC_2.34"
      "GLIBCXX_3.4.32"
      "libstdc++.so.6"
      "Linux 6.8 GA kernel"
      "Windows 11 x64 or newer"
      "Microsoft Visual C++ 2015-2022 Redistributable (x64)"
      "14.51.36231.0 or newer"
      "https://aka.ms/vs/17/release/vc_redist.x64.exe"
      "only `bennu build` requires an external C11 compiler")
    string(FIND "${document_text}" "${required_document_text}" found_at)
    if(found_at EQUAL -1)
      message(FATAL_ERROR
        "release deployment documentation is missing from ${document}: ${required_document_text}")
    endif()
  endforeach()
endforeach()

require_workflow_text("publish:\n    name: Verify and publish v0.1.0" "the publication job")
require_workflow_text("needs:\n      - package-linux\n      - package-native\n      - verify-linux-floor\n      - verify-linux-current" "publication dependency on every package and Linux runtime job")
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
    "published_release_endpoint="
    "release_api_endpoint=\"repos/\${repository}/releases/\${release_id}\""
    "(.id | type)"
    "created draft response metadata does not match"
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

set(publication_test "${BENNU_SOURCE_DIR}/tests/release_publication_test.py")
if(NOT EXISTS "${publication_test}")
  message(FATAL_ERROR "release publication state-machine test is missing: ${publication_test}")
endif()
file(READ "${publication_test}" publication_test_text)
foreach(required_test_text IN ITEMS
    "draft path queried the published-tag endpoint after creation"
    "malformed creation response"
    "asset upload failure"
    "matching published release was not a no-op"
    "tag not on live main"
    "public verification failure caused another mutation")
  string(FIND "${publication_test_text}" "${required_test_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "publication state-machine test is missing: ${required_test_text}")
  endif()
endforeach()

set(decision_diary "${BENNU_SOURCE_DIR}/doc/decision-diary.md")
file(READ "${decision_diary}" diary_text)
string(FIND "${diary_text}" "Gate v0.1.0 publication behind target-native verified archives" diary_entry_at)
if(diary_entry_at EQUAL -1)
  message(FATAL_ERROR "decision diary is missing the Issue #10 release topology decision")
endif()
string(FIND "${diary_text}" "Address draft Releases by creation ID until publication" draft_diary_entry_at)
if(draft_diary_entry_at EQUAL -1)
  message(FATAL_ERROR "decision diary is missing the Issue #20 draft endpoint decision")
endif()
string(FIND "${diary_text}" "Dynamically link the Windows release runtime" runtime_diary_entry_at)
if(runtime_diary_entry_at EQUAL -1)
  message(FATAL_ERROR "decision diary is missing the Issue #16 Windows runtime policy")
endif()
string(FIND "${diary_text}" "Support the durable Linux archive on Ubuntu 24.04 x64" linux_floor_diary_entry_at)
if(linux_floor_diary_entry_at EQUAL -1)
  message(FATAL_ERROR "decision diary is missing the Issue #13 Linux compatibility floor")
endif()
