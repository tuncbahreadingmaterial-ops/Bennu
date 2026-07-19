cmake_minimum_required(VERSION 3.20)

project(Bennu LANGUAGES CXX)

include(CTest)
if(BUILD_TESTING)
  enable_language(C)
endif()

add_library(bennu_core src/c_emitter.cpp src/evaluator.cpp)
target_include_directories(bennu_core PUBLIC include)
set_target_properties(bennu_core PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

add_executable(bennu src/main.cpp)
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
  add_cli_contract_test(unimplemented_build unimplemented_build)
endif()
