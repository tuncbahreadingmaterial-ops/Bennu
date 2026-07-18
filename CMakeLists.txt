cmake_minimum_required(VERSION 3.20)

project(Bennu LANGUAGES CXX)

add_executable(bennu src/main.cpp)
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
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bennu-build-info.txt"
  CONTENT "${bennu_build_info}"
)

include(CTest)
if(BUILD_TESTING)
  function(add_cli_contract_test name case_name)
    add_test(
      NAME "cli.${name}"
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENNU_EXECUTABLE=$<TARGET_FILE:bennu>"
        "-DCASE=${case_name}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_contract.cmake"
    )
  endfunction()

  add_cli_contract_test(help help)
  add_cli_contract_test(no_arguments no_arguments)
  add_cli_contract_test(unknown_option unknown_option)
  add_cli_contract_test(unknown_subcommand unknown_subcommand)
  add_cli_contract_test(unimplemented_repl unimplemented_repl)
  add_cli_contract_test(unimplemented_run unimplemented_run)
  add_cli_contract_test(unimplemented_emit_c unimplemented_emit_c)
  add_cli_contract_test(unimplemented_build unimplemented_build)
endif()
