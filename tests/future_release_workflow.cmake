if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

set(workflow "${BENNU_SOURCE_DIR}/.github/workflows/future-release.yml")
if(NOT EXISTS "${workflow}")
  message(FATAL_ERROR "future release workflow is missing")
endif()
file(READ "${workflow}" text)
file(READ
  "${BENNU_SOURCE_DIR}/tools/release/verify-clean-windows-package.ps1"
  windows_package_verifier)

function(require_text expected description)
  string(FIND "${text}" "${expected}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "future release workflow is missing ${description}: ${expected}")
  endif()
endfunction()

string(FIND "${text}" "pull_request" pull_request_at)
if(NOT pull_request_at EQUAL -1)
  message(FATAL_ERROR "future release workflow must not add pull-request triggers")
endif()
require_text("name: Future Release Candidate" "distinct workflow identity")
require_text("workflow_dispatch:" "manual trigger")
require_text("source_ref:" "explicit source ref input")
require_text("publish:" "explicit publication boolean")
require_text("release_tag:" "exact release tag input")
require_text("permissions:\n  contents: read" "read-only workflow default")
require_text("runner: ubuntu-24.04" "Linux x64 runner")
require_text("runner: windows-2022" "Windows x64 runner")
require_text("runner: macos-15" "macOS arm64 runner")
require_text("ctest --test-dir build --output-on-failure" "target-native complete CTest")
require_text("package-linux.sh" "Linux packaging")
require_text("verify-linux-package.sh" "Linux extracted journeys")
require_text("package-and-smoke.ps1" "Windows/macOS package journeys")
require_text("provenance.py fragment" "target-native fragment generation")
require_text("provenance.py merge" "canonical manifest merge")
require_text("provenance.py verify" "independent archive/manifest verification")
require_text("provenance.py gate" "stable annotated tag gate")
require_text([=[EVENT_SHA: ${{ github.sha }}]=] "attestation event source identity")
require_text([=[[ "$commit" != "$EVENT_SHA" ]]]=]
  "production checkout/event SHA equality gate")
require_text("actions/checkout@11d5960a326750d5838078e36cf38b85af677262" "pinned checkout")
require_text("actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02" "pinned upload-artifact")
require_text("actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093" "pinned download-artifact")
require_text("actions/attest-build-provenance@43d14bc2b83dec42d39ecae14e916627a18bb661" "pinned GitHub attestation")
require_text("contents: write\n      id-token: write\n      attestations: write" "publication-only privileges")
require_text("if: inputs.publish" "publication boolean job gate")
require_text("publish-future-release.sh" "fail-closed publication state machine")
require_text("bennu-v0.1.0 assets are immutable" "explicit historical-version rejection evidence")
require_text([=[RELEASE_TAG: ${{ inputs.release_tag }}]=]
  "release tag passed through the step environment")
require_text([=[--tag "$RELEASE_TAG"]=]
  "shell-safe production gate tag argument")
require_text([=["$RELEASE_TAG"]=]
  "shell-safe publisher tag argument")
foreach(forbidden_input IN ITEMS
    [=[--tag '${{ inputs.release_tag }}']=]
    [=['${{ inputs.release_tag }}']=])
  string(FIND "${text}" "${forbidden_input}" unsafe_input_at)
  if(NOT unsafe_input_at EQUAL -1)
    message(FATAL_ERROR
      "future release workflow interpolates release_tag directly into shell: ${forbidden_input}")
  endif()
endforeach()

string(FIND "${windows_package_verifier}"
  "       bennu --version"
  windows_help_version_line)
if(windows_help_version_line EQUAL -1)
  message(FATAL_ERROR
    "clean Windows package help contract is missing bennu --version")
endif()

set(publisher "${BENNU_SOURCE_DIR}/tools/release/publish-future-release.sh")
file(READ "${publisher}" publisher_text)
foreach(forbidden IN ITEMS
    "gh release create"
    "gh release upload"
    "gh release download"
    "gh release view"
    "gh release edit"
    "gh release delete"
    "--hostname uploads.github.com"
    [=["repos/${repository}/releases/${release_id}/assets?name=${name}"]=])
  string(FIND "${publisher_text}" "${forbidden}" forbidden_at)
  if(NOT forbidden_at EQUAL -1)
    message(FATAL_ERROR "future publisher must not use draft tag lookup through: ${forbidden}")
  endif()
endforeach()
foreach(required IN ITEMS
    "gh api --method POST \"repos/\${repository}/releases\""
    "release_api_endpoint=\"repos/\${repository}/releases/\${release_id}\""
    [=[remote_field '.upload_url']=]
    "https://uploads.github.com/"
    [=["${upload_endpoint}?name=${name}"]=]
    [=[[[ ! "$resume_release_id" =~ ^[0-9]+$ ]]]=]
    [=[[[ "$(remote_field '.name')" == "$release_name" ]]]=]
    "repos/\${repository}/releases/assets/\${asset_id}"
    "gh api --method PATCH \"\$release_api_endpoint\""
    "-F draft=false")
  string(FIND "${publisher_text}" "${required}" required_at)
  if(required_at EQUAL -1)
    message(FATAL_ERROR "future publisher is missing numeric release-ID REST contract: ${required}")
  endif()
endforeach()
