cmake_minimum_required(VERSION 3.20)

project(Bennu LANGUAGES CXX)

include(CTest)
if(BUILD_TESTING)
  enable_language(C)
endif()

add_library(bennu_core src/c_emitter.cpp src/evaluator.cpp src/native_builder.cpp)
target_include_directories(bennu_core PUBLIC include)
set_target_properties(bennu_core PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

add_executable(bennu src/cli_output.cpp src/main.cpp)
target_include_directories(bennu PRIVATE src)
target_link_libraries(bennu PRIVATE bennu_core)
set_target_properties(bennu PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

string(CONCAT bennu_build_info
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

if(BUILD_TESTING)
  add_executable(bennu_evaluator_tests tests/evaluator_test.cpp)
  target_link_libraries(bennu_evaluator_tests PRIVATE bennu_core)
  set_target_properties(bennu_evaluator_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  add_test(NAME evaluator.unit COMMAND bennu_evaluator_tests)

  add_executable(bennu_c_emitter_tests tests/c_emitter_test.cpp)
  target_link_libraries(bennu_c_emitter_tests PRIVATE bennu_core)
  set_target_properties(bennu_c_emitter_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  add_test(NAME c_emitter.unit COMMAND bennu_c_emitter_tests)

  add_executable(bennu_native_builder_tests tests/native_builder_test.cpp)
  target_link_libraries(bennu_native_builder_tests PRIVATE bennu_core)
  set_target_properties(bennu_native_builder_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  add_test(NAME native_builder.unit COMMAND bennu_native_builder_tests)

  add_executable(bennu_stdout_tests tests/stdout_test.cpp src/cli_output.cpp)
  target_include_directories(bennu_stdout_tests PRIVATE src)
  set_target_properties(bennu_stdout_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  add_test(NAME stdout.unit COMMAND bennu_stdout_tests)

  add_executable(bennu_fake_c_compiler tests/fake_c_compiler.cpp)
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
        "-DCASE=${case_name}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_contract.cmake"
    )
  endfunction()

  add_cli_contract_test(help help)
  add_cli_contract_test(no_arguments no_arguments)
  add_cli_contract_test(unknown_option unknown_option)
  add_cli_contract_test(unknown_subcommand unknown_subcommand)
  add_cli_contract_test(repl_transcript repl_transcript)
  add_cli_contract_test(repl_eof repl_eof)
  add_cli_contract_test(repl_recovers_after_errors repl_recovers_after_errors)
  add_cli_contract_test(repl_ignores_blank_lines repl_ignores_blank_lines)
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
    NAME documentation.level1_journeys
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DBENNU_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DBENNU_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}"
      "-DBENNU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/documentation_smoke.cmake"
  )
  add_test(
    NAME release.workflow_contract
    COMMAND
      "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_workflow.cmake"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(BENNU_BASH_EXECUTABLE bash REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
      NAME release.publication_state_machine
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_publication_test.py"
        "${BENNU_BASH_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/publish-release.sh"
    )
  endif()
endif()
