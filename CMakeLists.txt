cmake_minimum_required(VERSION 3.20)

project(Bennu LANGUAGES CXX)

set(BENNU_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/bennu_version.cmake")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/private")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bennu_version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/private/bennu_version.hpp"
  @ONLY
)

if(WIN32)
  enable_language(RC)
  set(BENNU_MANIFEST_RESOURCE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bennu.exe.manifest")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bennu-version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/private/bennu-version.rc"
    @ONLY
  )
endif()

option(BENNU_BUILD_ISSUE33_BENCHMARK
  "Expose the opt-in Issue #33 C-backend prototype and benchmark targets"
  OFF
)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

include(CTest)
if(BUILD_TESTING)
  enable_language(C)
endif()

set(bennu_implementation_sources
  src/application.cpp
  src/error.cpp
  src/native_builder.cpp
  src/path_encoding.cpp
  src/primitive.cpp
  src/rewrite.cpp
  src/rewrite_c_runtime.cpp
  src/resources.cpp
  src/type.cpp
  src/value.cpp
)

if(MSVC)
  set_source_files_properties(src/primitive.cpp PROPERTIES
    COMPILE_OPTIONS "/fp:strict"
  )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/primitive.cpp PROPERTIES
    COMPILE_OPTIONS "-frounding-math;-fno-fast-math"
  )
endif()

add_executable(bennu
  ${bennu_implementation_sources}
  src/cli_output.cpp
  src/main.cpp
)
if(WIN32)
  target_sources(bennu PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/private/bennu-version.rc"
  )
endif()
if(MSVC)
  target_link_options(bennu PRIVATE "/MANIFEST:NO")
endif()
target_include_directories(bennu PRIVATE
  include
  src
  "${CMAKE_CURRENT_BINARY_DIR}/generated/private"
)
target_include_directories(bennu SYSTEM PRIVATE third_party/doctest)
target_compile_definitions(bennu PRIVATE
  DOCTEST_CONFIG_DISABLE
  DOCTEST_CONFIG_NO_EXCEPTIONS
  DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
)
set_target_properties(bennu PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

string(CONCAT bennu_build_info
  "version=${BENNU_VERSION}\n"
  "system=${CMAKE_SYSTEM_NAME}\n"
  "architecture=${CMAKE_SYSTEM_PROCESSOR}\n"
  "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
  "cmake=${CMAKE_VERSION}\n"
)
if(BUILD_TESTING)
  string(APPEND bennu_build_info
    "c_compiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\n")
endif()
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bennu-build-info.txt"
  CONTENT "${bennu_build_info}"
)

if(BENNU_BUILD_ISSUE33_BENCHMARK)
  enable_language(C)
  if(NOT UNIX OR NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "Issue #33 measurement requires a Unix host and a configured GNU- or Clang-compatible C compiler"
    )
  endif()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  add_executable(bennu_issue33_prototype EXCLUDE_FROM_ALL
    tools/issue33/prototype.cpp
    src/application.cpp
    src/error.cpp
    src/primitive.cpp
    src/resources.cpp
    src/value.cpp
  )
  target_include_directories(bennu_issue33_prototype PRIVATE include src)
  target_include_directories(bennu_issue33_prototype SYSTEM PRIVATE
    third_party/doctest
  )
  target_compile_definitions(bennu_issue33_prototype PRIVATE
    DOCTEST_CONFIG_DISABLE
    DOCTEST_CONFIG_NO_EXCEPTIONS
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
  )
  set_target_properties(bennu_issue33_prototype PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )

  set(bennu_issue33_driver
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/issue33/benchmark.py"
  )
  set(bennu_issue33_prototype_source
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/issue33/prototype.cpp"
  )

  add_custom_target(issue33_verify
    COMMAND
      "${Python3_EXECUTABLE}" "${bennu_issue33_driver}" verify
      --prototype "$<TARGET_FILE:bennu_issue33_prototype>"
      --prototype-source "${bennu_issue33_prototype_source}"
      --cc "${CMAKE_C_COMPILER}"
      --c-flag=-std=c11
      --c-flag=-O2
      --c-flag=-Wall
      --c-flag=-Wextra
      --c-flag=-Werror
      --c-flag=-pedantic-errors
    DEPENDS bennu_issue33_prototype
    USES_TERMINAL
    VERBATIM
  )

  add_custom_target(issue33_measure
    COMMAND
      "${Python3_EXECUTABLE}" "${bennu_issue33_driver}" measure
      --prototype "$<TARGET_FILE:bennu_issue33_prototype>"
      --prototype-source "${bennu_issue33_prototype_source}"
      --cc "${CMAKE_C_COMPILER}"
      --c-flag=-std=c11
      --c-flag=-O2
      --c-flag=-Wall
      --c-flag=-Wextra
      --c-flag=-Werror
      --c-flag=-pedantic-errors
      --source-dir "${CMAKE_CURRENT_SOURCE_DIR}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/issue33-evidence"
      --samples 5
      --build-type "$<CONFIG>"
      --cmake-version "${CMAKE_VERSION}"
      --c-compiler-id "${CMAKE_C_COMPILER_ID}"
      --c-compiler-version "${CMAKE_C_COMPILER_VERSION}"
      --cxx-compiler "${CMAKE_CXX_COMPILER}"
      --cxx-compiler-id "${CMAKE_CXX_COMPILER_ID}"
      --cxx-compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
      --cxx-flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE} -std=c++20 -DDOCTEST_CONFIG_DISABLE -DDOCTEST_CONFIG_NO_EXCEPTIONS -DDOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS [src/primitive.cpp: -frounding-math -fno-fast-math]"
    DEPENDS bennu_issue33_prototype
    USES_TERMINAL
    VERBATIM
  )
endif()

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(bennu_tests
    ${bennu_implementation_sources}
    tests/application_section16_conformance.cpp
    tests/doctest_main.cpp
    tests/public_cutover_conformance.cpp
    tests/tuple_foundation_conformance.cpp
  )
  target_include_directories(bennu_tests PRIVATE include)
  target_include_directories(bennu_tests SYSTEM PRIVATE third_party/doctest)
  target_compile_definitions(bennu_tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
  )
  set_target_properties(bennu_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  add_test(NAME unit.doctest COMMAND bennu_tests)
  add_test(
    NAME spec.traceability
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/spec_traceability.cmake"
  )
  add_test(
    NAME spec.traceability_bounded_once_negative
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DBENNU_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/spec_traceability_negative.cmake"
  )
  add_test(
    NAME architecture.obsolete_surface
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/obsolete_surface_contract.cmake"
  )
  add_test(
    NAME architecture.doctest_topology
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/doctest_topology.cmake"
  )

  add_executable(bennu_stdout_tests tests/stdout_test.cpp src/cli_output.cpp)
  target_include_directories(bennu_stdout_tests PRIVATE src)
  set_target_properties(bennu_stdout_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  add_test(NAME stdout.unit COMMAND bennu_stdout_tests)

  add_executable(bennu_public_resource_fixture
    ${bennu_implementation_sources}
    tests/public_resource_fixture.cpp
  )
  target_include_directories(bennu_public_resource_fixture PRIVATE include)
  target_include_directories(bennu_public_resource_fixture SYSTEM PRIVATE
    third_party/doctest
  )
  target_compile_definitions(bennu_public_resource_fixture PRIVATE
    DOCTEST_CONFIG_DISABLE
    DOCTEST_CONFIG_NO_EXCEPTIONS
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
  )
  set_target_properties(bennu_public_resource_fixture PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )

  add_executable(bennu_fake_c_compiler
    tests/fake_c_compiler.cpp
    src/path_encoding.cpp
  )
  target_include_directories(bennu_fake_c_compiler PRIVATE include)
  set_target_properties(bennu_fake_c_compiler PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )

  function(add_cli_contract_test name case_name)
    add_test(
      NAME "cli.${name}"
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
        "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBENNU_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DBENNU_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}"
        "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
        "-DBENNU_VERSION=${BENNU_VERSION}"
        "-DCASE=${case_name}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_contract.cmake"
    )
  endfunction()

  add_cli_contract_test(help help)
  add_cli_contract_test(version version)
  add_cli_contract_test(no_arguments no_arguments)
  add_cli_contract_test(unknown_option unknown_option)
  add_cli_contract_test(unknown_subcommand unknown_subcommand)
  add_cli_contract_test(repl_transcript repl_transcript)
  add_cli_contract_test(repl_eof repl_eof)
  add_cli_contract_test(repl_recovers_after_errors repl_recovers_after_errors)
  add_cli_contract_test(repl_ignores_blank_lines repl_ignores_blank_lines)
  add_cli_contract_test(repl_rejects_parameter_header repl_rejects_parameter_header)
  add_cli_contract_test(repl_rejects_arguments repl_rejects_arguments)
  add_cli_contract_test(run_example run_example)
  add_cli_contract_test(run_path_with_spaces run_path_with_spaces)
  add_cli_contract_test(run_crlf_and_blank_lines run_crlf_and_blank_lines)
  add_cli_contract_test(run_missing_path run_missing_path)
  add_cli_contract_test(run_extra_argument run_extra_argument)
  add_cli_contract_test(run_nonexistent_file run_nonexistent_file)
  add_cli_contract_test(run_directory_path run_directory_path)
  if(UNIX)
    add_cli_contract_test(run_unreadable_file run_unreadable_file)
  endif()
  add_cli_contract_test(run_evaluator_errors run_evaluator_errors)
  add_cli_contract_test(run_batch_no_partial_output run_batch_no_partial_output)
  add_cli_contract_test(emit_c_example emit_c_example)
  add_cli_contract_test(emit_c_empty_program emit_c_empty_program)
  add_test(
    NAME cli.emit_c_differential
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DBENNU_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DBENNU_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}"
      "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_emission_contract.cmake"
  )
  add_test(
    NAME cli.public_path_error_matrix
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
      "-DBENNU_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DBENNU_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}"
      "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/public_path_error_matrix.cmake"
  )
  add_test(
    NAME public.api_resource_matrix
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_PUBLIC_RESOURCE_FIXTURE=$<TARGET_FILE:bennu_public_resource_fixture>"
      "-DBENNU_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DBENNU_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}"
      "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/public_resource_contract.cmake"
  )
  add_test(
    NAME cli.emit_c_negative_atomic
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_emission_negative.cmake"
  )
  if(UNIX)
    add_cli_contract_test(emit_c_unwritable_atomic emit_c_unwritable_atomic)
  endif()
  add_cli_contract_test(build_missing_source build_missing_source)
  add_cli_contract_test(build_invalid_arguments build_invalid_arguments)
  add_test(
    NAME docs.smoke
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/documentation_smoke.cmake"
  )
  add_test(
    NAME cli.build_native
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DBENNU_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_contract.cmake"
  )
  if(WIN32)
    set(bennu_fallback_compiler_name "cl.exe")
  else()
    set(bennu_fallback_compiler_name "cc")
  endif()
  add_test(
    NAME cli.build_fake_process
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
      "-DBENNU_FAKE_COMPILER=$<TARGET_FILE:bennu_fake_c_compiler>"
      "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DBENNU_FALLBACK_COMPILER_NAME=${bennu_fallback_compiler_name}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_fake_process.cmake"
  )
  if(WIN32)
    add_test(
      NAME release.windows_version_resource
      COMMAND pwsh -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_version_resource.ps1"
        -BennuExecutable "$<TARGET_FILE:bennu>"
        -ExpectedVersion "${BENNU_VERSION}"
    )
    add_test(
      NAME cli.windows_unicode_paths
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
        "-DBENNU_FAKE_COMPILER=$<TARGET_FILE:bennu_fake_c_compiler>"
        "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_unicode_paths.cmake"
    )
    add_test(
      NAME cli.windows_long_paths
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
        "-DBENNU_FAKE_COMPILER=$<TARGET_FILE:bennu_fake_c_compiler>"
        "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
        "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_long_paths.cmake"
    )
  endif()
  add_test(
    NAME cli.source_output_alias
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
      "-DBENNU_FAKE_COMPILER=$<TARGET_FILE:bennu_fake_c_compiler>"
      "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_output_alias_contract.cmake"
  )
  if(EXISTS "/dev/full")
    add_test(
      NAME cli.stdout_failure
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
        "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/stdout_failure_contract.cmake"
    )
  endif()
  add_test(
    NAME workflow.main_contract
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/main_workflow.cmake"
  )
  add_test(
    NAME workflow.main_contract_negative
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DBENNU_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/main_workflow_negative.cmake"
  )
  add_test(
    NAME release.workflow_contract
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_workflow.cmake"
  )
  add_test(
    NAME release.version_source_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DBENNU_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DBENNU_PYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/version_source_contract.cmake"
  )
  add_test(
    NAME release.stable_version_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/stable_version_contract.cmake"
  )
  add_test(
    NAME release.windows_version_resource_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_version_resource_contract.cmake"
  )
  add_test(
    NAME release.windows_long_path_policy_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_long_path_policy_contract.cmake"
  )
  if(WIN32)
    set(bennu_test_release_target windows-x64)
  elseif(APPLE)
    set(bennu_test_release_target macos-arm64)
  else()
    set(bennu_test_release_target linux-x64)
  endif()
  add_test(
    NAME release.provenance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_provenance_test.py"
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/provenance.py"
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "$<TARGET_FILE:bennu>"
      "${bennu_test_release_target}"
  )
  add_test(
    NAME release.future_workflow_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/future_release_workflow.cmake"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(BENNU_BASH_EXECUTABLE bash REQUIRED)
    add_test(
      NAME release.linux_compatibility
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_compatibility_test.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/verify-linux-elf.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        "$<TARGET_FILE:bennu>"
    )
    add_test(
      NAME release.linux_package
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
        "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBENNU_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DBENNU_BASH_EXECUTABLE=${BENNU_BASH_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_package_contract.cmake"
    )
    add_test(
      NAME release.publication_state_machine
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_publication_test.py"
        "${BENNU_BASH_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/publish-release.sh"
    )
    add_test(
      NAME release.future_publication_state_machine
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/future_release_publication_test.py"
        "${BENNU_BASH_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/publish-future-release.sh"
    )
  endif()
endif()
