foreach(required BENNU_SOURCE_DIR BENNU_BINARY_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_root "${BENNU_BINARY_DIR}/version-source-contract")
file(REMOVE_RECURSE "${work_root}")
file(MAKE_DIRECTORY "${work_root}")
set(version_parser "${BENNU_SOURCE_DIR}/cmake/bennu_version.cmake")

file(READ "${BENNU_SOURCE_DIR}/tests/cli_contract.cmake" cli_contract)
string(FIND "${cli_contract}" [=[set(expected_stdout "bennu ${BENNU_VERSION}\n")]=]
  configured_cli_version_at)
if(configured_cli_version_at EQUAL -1)
  message(FATAL_ERROR "CLI version contract is not derived from configured BENNU_VERSION")
endif()
string(FIND "${cli_contract}" "bennu 0.2.0-dev" hard_coded_cli_version_at)
if(NOT hard_coded_cli_version_at EQUAL -1)
  message(FATAL_ERROR "CLI version contract hard-codes the current development version")
endif()

file(READ "${BENNU_SOURCE_DIR}/CMakeLists.txt" root_cmake)
string(FIND "${root_cmake}" [=["-DBENNU_VERSION=${BENNU_VERSION}"]=]
  passed_cli_version_at)
if(passed_cli_version_at EQUAL -1)
  message(FATAL_ERROR "CTest does not pass configured BENNU_VERSION to the CLI contract")
endif()

file(READ "${BENNU_SOURCE_DIR}/tests/documentation_smoke.cmake" documentation_smoke)
string(FIND "${documentation_smoke}" [=[file(READ "${BENNU_SOURCE_DIR}/VERSION" product_version)]=]
  documentation_version_read_at)
string(FIND "${documentation_smoke}" [=["`${product_version}`;"]=]
  documentation_exact_version_at)
string(FIND "${documentation_smoke}" "\"0.2.0-dev\"" hard_coded_documentation_version_at)
if(documentation_version_read_at EQUAL -1 OR documentation_exact_version_at EQUAL -1)
  message(FATAL_ERROR "documentation smoke does not derive the documented product version from VERSION")
endif()
if(NOT hard_coded_documentation_version_at EQUAL -1)
  message(FATAL_ERROR "documentation smoke hard-codes the current development version")
endif()

function(check_version name contents expected_status)
  set(version_file "${work_root}/${name}.txt")
  file(WRITE "${version_file}" "${contents}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_VERSION_FILE=${version_file}"
      -P "${version_parser}"
    RESULT_VARIABLE actual_status
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
  )
  if(expected_status STREQUAL "success")
    if(NOT actual_status EQUAL 0)
      message(FATAL_ERROR
        "${name}: valid version was rejected (${actual_status})\n${actual_stdout}${actual_stderr}")
    endif()
  elseif(actual_status EQUAL 0)
    message(FATAL_ERROR "${name}: invalid version was accepted")
  endif()
endfunction()

check_version(stable "1.2.3\n" success)
check_version(development "0.2.0-dev\n" success)
check_version(build_metadata "2.4.6-rc.1+build.9\n" success)
check_version(no_newline "1.2.3" failure)
check_version(crlf "1.2.3\r\n" failure)
check_version(extra_line "1.2.3\n2.0.0\n" failure)
check_version(leading_zero_core "01.2.3\n" failure)
check_version(empty_prerelease "1.2.3-\n" failure)
check_version(leading_zero_prerelease "1.2.3-01\n" failure)
check_version(pe_component_overflow "1.2.65536\n" failure)

file(REMOVE_RECURSE "${work_root}")
