set(main_workflow "${BENNU_SOURCE_DIR}/.github/workflows/main.yml")

if(NOT EXISTS "${main_workflow}")
  message(FATAL_ERROR "Main CI workflow is missing: ${main_workflow}")
endif()

file(READ "${main_workflow}" workflow_text)

function(require_workflow_text expected description)
  string(FIND "${workflow_text}" "${expected}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "Main CI is missing ${description}: ${expected}")
  endif()
endfunction()

function(require_workflow_count expected_count pattern description)
  string(REGEX MATCHALL "${pattern}" matches "${workflow_text}")
  list(LENGTH matches actual_count)
  if(NOT actual_count EQUAL expected_count)
    message(FATAL_ERROR
      "Main CI must contain ${expected_count} ${description}; found ${actual_count}")
  endif()
endfunction()

require_workflow_text([=[on:
  pull_request:
    branches:
      - main
  push:
    branches:
      - main
  workflow_dispatch:]=]
  "pull-request, post-merge push, and manual triggers")
require_workflow_text([=[concurrency:
  group: ${{ github.workflow }}-${{ github.event.pull_request.number || github.run_id }}
  cancel-in-progress: ${{ github.event_name == 'pull_request' }}]=]
  "per-PR superseded-revision cancellation without grouping push runs")
require_workflow_text("permissions: {}" "a no-permission workflow default")
require_workflow_text([=[build-and-test:
    name: ${{ matrix.name }}
    runs-on: ${{ matrix.runner }}
    timeout-minutes: 15
    permissions:
      contents: read]=]
  "read-only source access limited to the build matrix")

foreach(forbidden_permission IN ITEMS
    "contents: write"
    "actions: write"
    "checks: write"
    "pull-requests: write"
    "statuses: write")
  string(FIND "${workflow_text}" "${forbidden_permission}" permission_at)
  if(NOT permission_at EQUAL -1)
    message(FATAL_ERROR
      "Main CI must not grant write authority: ${forbidden_permission}")
  endif()
endforeach()

require_workflow_count(2 "permissions:" "permission declarations")
require_workflow_count(1 "contents: read" "read-only contents grants")

require_workflow_text([=[- name: Linux x64
            runner: ubuntu-24.04
            expected_os: Linux
            expected_arch: X64]=]
  "the Linux x64 matrix entry")
require_workflow_text([=[- name: Windows x64
            runner: windows-2022
            expected_os: Windows
            expected_arch: X64]=]
  "the Windows x64 matrix entry")
require_workflow_text([=[- name: macOS arm64
            runner: macos-15
            expected_os: macOS
            expected_arch: Arm64]=]
  "the macOS arm64 matrix entry")
require_workflow_count(1 "runner: ubuntu-24\\.04" "Linux x64 matrix entries")
require_workflow_count(1 "runner: windows-2022" "Windows x64 matrix entries")
require_workflow_count(1 "runner: macos-15" "macOS arm64 matrix entries")
require_workflow_text(
  "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
  "the Release configuration")
require_workflow_text("cmake --build build" "the complete build")
require_workflow_text(
  "ctest --test-dir build --output-on-failure"
  "the complete CTest suite")

require_workflow_text([=[pr-gate:
    name: PR Gate
    if: ${{ always() }}
    needs: build-and-test
    runs-on: ubuntu-24.04
    timeout-minutes: 2]=]
  "the stable aggregate gate topology")
require_workflow_text([=[env:
          MATRIX_RESULT: ${{ needs.build-and-test.result }}
        run: test "$MATRIX_RESULT" = "success"]=]
  "an aggregate failure unless the complete matrix succeeds")
require_workflow_count(1 "name: PR Gate" "stable PR Gate checks")
