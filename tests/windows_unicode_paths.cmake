foreach(required BENNU_EXECUTABLE BENNU_FAKE_COMPILER BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work "${CMAKE_CURRENT_BINARY_DIR}/Windows Unicode space éß 文 🐍")
set(source "${work}/source space éß 文 🐍.bennu")
set(invalid_source "${work}/invalid source 文 🐍.bennu")
set(missing_source "${work}/missing source 文 🐍.bennu")
set(emitted "${work}/emitted space éß 文 🐍.c")
set(output_directory "${work}/output directory 文 🐍")
set(compiler_directory "${work}/compiler directory 文 🐍")
set(fake_compiler "${compiler_directory}/cl.exe")
set(trace "${work}/compiler trace 文 🐍.txt")
set(native_output "${work}/native output 文 🐍${BENNU_EXECUTABLE_SUFFIX}")
set(cc_output "${work}/CC output 文 🐍${BENNU_EXECUTABLE_SUFFIX}")

file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}" "${output_directory}" "${compiler_directory}")
file(WRITE "${source}" "iota[5]\nadd[1 2.5]\n")
file(WRITE "${invalid_source}" "inc 5\nwat\n")
configure_file("${BENNU_FAKE_COMPILER}" "${fake_compiler}" COPYONLY)

function(assert_no_temporary_artifacts context)
  file(GLOB_RECURSE leftovers
    "${work}/*.tmp" "${work}/*.tmp.*"
    "${work}/*.bennu-build.tmp*" "${work}/*.bennu-native.tmp*")
  if(leftovers)
    message(FATAL_ERROR "${context}: temporary artifacts remain: ${leftovers}")
  endif()
endfunction()

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${source}"
  RESULT_VARIABLE run_exit OUTPUT_VARIABLE run_stdout ERROR_VARIABLE run_stderr)
string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
if(NOT "${run_exit}" STREQUAL "0" OR
   NOT run_stdout STREQUAL "(1 2 3 4 5)\n3.5\n" OR
   NOT run_stderr STREQUAL "")
  message(FATAL_ERROR
    "Unicode run failed\nexit: ${run_exit}\nstdout: [${run_stdout}]\nstderr: [${run_stderr}]")
endif()

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${missing_source}"
  RESULT_VARIABLE missing_exit OUTPUT_VARIABLE missing_stdout
  ERROR_VARIABLE missing_stderr)
string(REPLACE "\r\n" "\n" missing_stderr "${missing_stderr}")
if("${missing_exit}" STREQUAL "0" OR NOT missing_stdout STREQUAL "" OR
   NOT missing_stderr STREQUAL
       "${missing_source}:1:1: file error: unable to read source\n")
  message(FATAL_ERROR
    "Unicode missing-source diagnostic mismatch\nexit: ${missing_exit}\n"
    "stdout: [${missing_stdout}]\nstderr: [${missing_stderr}]")
endif()

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source}" -o "${emitted}"
  RESULT_VARIABLE emit_exit OUTPUT_VARIABLE emit_stdout ERROR_VARIABLE emit_stderr)
if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
   NOT emit_stderr STREQUAL "" OR NOT EXISTS "${emitted}")
  message(FATAL_ERROR
    "Unicode emit-c failed\nexit: ${emit_exit}\nstdout: [${emit_stdout}]\n"
    "stderr: [${emit_stderr}]")
endif()
file(READ "${emitted}" emitted_source)
if(NOT emitted_source MATCHES "#include <inttypes.h>" OR
   NOT emitted_source MATCHES "bennu_print_array")
  message(FATAL_ERROR "Unicode emit-c produced unexpected C")
endif()

file(WRITE "${emitted}" "sentinel bytes\n")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${invalid_source}" -o "${emitted}"
  RESULT_VARIABLE invalid_exit OUTPUT_VARIABLE invalid_stdout
  ERROR_VARIABLE invalid_stderr)
string(REPLACE "\r\n" "\n" invalid_stderr "${invalid_stderr}")
file(READ "${emitted}" preserved_emitted)
if("${invalid_exit}" STREQUAL "0" OR NOT invalid_stdout STREQUAL "" OR
   NOT invalid_stderr STREQUAL
       "${invalid_source}:2:1: unknown name: unknown name: wat\n" OR
   NOT preserved_emitted STREQUAL "sentinel bytes\n")
  message(FATAL_ERROR
    "Unicode emit-c semantic failure was not atomic\nexit: ${invalid_exit}\n"
    "stdout: [${invalid_stdout}]\nstderr: [${invalid_stderr}]\n"
    "output: [${preserved_emitted}]")
endif()
assert_no_temporary_artifacts("Unicode emit-c semantic failure")

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source}" -o "${output_directory}"
  RESULT_VARIABLE publish_exit OUTPUT_VARIABLE publish_stdout
  ERROR_VARIABLE publish_stderr)
string(REPLACE "\r\n" "\n" publish_stderr "${publish_stderr}")
if("${publish_exit}" STREQUAL "0" OR NOT publish_stdout STREQUAL "" OR
   NOT publish_stderr STREQUAL
       "${output_directory}:1:1: file error: unable to write output\n" OR
   NOT IS_DIRECTORY "${output_directory}")
  message(FATAL_ERROR
    "Unicode emit-c publication failure mismatch\nexit: ${publish_exit}\n"
    "stdout: [${publish_stdout}]\nstderr: [${publish_stderr}]")
endif()
assert_no_temporary_artifacts("Unicode emit-c publication failure")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${source}" -o "${native_output}"
          --cc "${fake_compiler}"
  RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout ERROR_VARIABLE build_stderr)
if(NOT "${build_exit}" STREQUAL "0" OR NOT build_stdout STREQUAL "" OR
   NOT build_stderr STREQUAL "" OR NOT EXISTS "${native_output}" OR
   NOT EXISTS "${trace}")
  message(FATAL_ERROR
    "Unicode explicit compiler build failed\nexit: ${build_exit}\n"
    "stdout: [${build_stdout}]\nstderr: [${build_stderr}]")
endif()
file(READ "${trace}" explicit_trace)
if(NOT explicit_trace MATCHES "/std:c11" OR
   NOT explicit_trace MATCHES "Windows Unicode space éß 文 🐍" OR
   NOT explicit_trace MATCHES "/Fe:.*program.exe")
  message(FATAL_ERROR "Unicode compiler argument boundaries mismatch: ${explicit_trace}")
endif()
assert_no_temporary_artifacts("Unicode explicit compiler build")

file(REMOVE "${trace}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "CC=${fake_compiler}" "BENNU_FAKE_CC_MODE=success"
          "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${source}" -o "${cc_output}"
  RESULT_VARIABLE cc_exit OUTPUT_VARIABLE cc_stdout ERROR_VARIABLE cc_stderr)
if(NOT "${cc_exit}" STREQUAL "0" OR NOT cc_stdout STREQUAL "" OR
   NOT cc_stderr STREQUAL "" OR NOT EXISTS "${cc_output}" OR
   NOT EXISTS "${trace}")
  message(FATAL_ERROR
    "Unicode CC compiler build failed\nexit: ${cc_exit}\n"
    "stdout: [${cc_stdout}]\nstderr: [${cc_stderr}]")
endif()
assert_no_temporary_artifacts("Unicode CC compiler build")

file(WRITE "${native_output}" "sentinel native bytes\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BENNU_FAKE_CC_MODE=fail"
          "${BENNU_EXECUTABLE}" build "${source}" -o "${native_output}"
          --cc "${fake_compiler}"
  RESULT_VARIABLE failure_exit OUTPUT_VARIABLE failure_stdout
  ERROR_VARIABLE failure_stderr)
file(READ "${native_output}" preserved_native)
if("${failure_exit}" STREQUAL "0" OR NOT failure_stdout STREQUAL "" OR
   NOT failure_stderr MATCHES "selected from --cc.*compiler directory 文 🐍.*exited with status 23" OR
   NOT preserved_native STREQUAL "sentinel native bytes\n")
  message(FATAL_ERROR
    "Unicode compiler failure was not atomic\nexit: ${failure_exit}\n"
    "stdout: [${failure_stdout}]\nstderr: [${failure_stderr}]\n"
    "output: [${preserved_native}]")
endif()
assert_no_temporary_artifacts("Unicode compiler failure")

file(REMOVE_RECURSE "${work}")
