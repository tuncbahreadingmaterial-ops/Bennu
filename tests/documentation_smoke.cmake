foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

function(require_documented_text path description expected)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${description}: missing ${path}")
  endif()
  file(READ "${path}" contents)
  string(REPLACE "\r\n" "\n" contents "${contents}")
  string(FIND "${contents}" "${expected}" offset)
  if(offset EQUAL -1)
    message(FATAL_ERROR
      "${description}: ${path} does not contain [${expected}]")
  endif()
endfunction()

set(readme "${BENNU_SOURCE_DIR}/README.md")
set(level1_document "${BENNU_SOURCE_DIR}/doc/level1.md")
set(example "${BENNU_SOURCE_DIR}/examples/level1.bennu")
set(expected_source "ioata 5\ninc 5\n")
set(expected_output ">>(1 2 3 4 5)\n>>6\n")

file(READ "${example}" actual_source)
string(REPLACE "\r\n" "\n" actual_source "${actual_source}")
if(NOT actual_source STREQUAL expected_source)
  message(FATAL_ERROR
    "canonical example drifted\nexpected: [${expected_source}]\n"
    "actual:   [${actual_source}]")
endif()

require_documented_text("${readme}" "README quick start"
  "[Level 1 language and toolchain](doc/level1.md)")
require_documented_text("${readme}" "README quick start" "`ioata n`")
require_documented_text("${readme}" "README quick start" "`inc n`")
require_documented_text("${readme}" "README quick start" "Linux x64")
require_documented_text("${readme}" "README quick start"
  "Windows 11 x64 or newer")
require_documented_text("${readme}" "README quick start" "macOS arm64")
require_documented_text("${readme}" "README quick start"
  "./build/bennu repl")
require_documented_text("${readme}" "README quick start"
  "./build/bennu run examples/level1.bennu")
require_documented_text("${readme}" "README quick start"
  "./build/bennu emit-c examples/level1.bennu -o level1.c")
require_documented_text("${readme}" "README quick start"
  "./build/bennu build examples/level1.bennu -o level1")
require_documented_text("${readme}" "README quick start" "${expected_output}")

require_documented_text("${level1_document}" "Level 1 contract"
  "-9223372036854775808")
require_documented_text("${level1_document}" "Level 1 contract"
  "9223372036854775807")
require_documented_text("${level1_document}" "Level 1 contract" "1,000,000")
require_documented_text("${level1_document}" "Level 1 contract" "nested")
require_documented_text("${level1_document}" "Level 1 contract" "blank")
require_documented_text("${level1_document}" "Level 1 contract" "wrong type")
require_documented_text("${level1_document}" "Level 1 contract" "`--cc`")
require_documented_text("${level1_document}" "Level 1 contract" "`CC`")
require_documented_text("${level1_document}" "Level 1 contract" "`cc`")
require_documented_text("${level1_document}" "Level 1 contract" "`cl.exe`")
require_documented_text("${level1_document}" "Level 1 contract"
  "does not bundle, download, or install a C compiler")
require_documented_text("${level1_document}" "Level 1 contract" "Anka")
require_documented_text("${level1_document}" "Level 1 contract"
  "array-lifted `inc`")
require_documented_text("${level1_document}" "Level 1 contract"
  "bennu-v0.1.0-linux-x64.tar.gz")
require_documented_text("${level1_document}" "Level 1 contract"
  "bennu-v0.1.0-windows-x64.zip")
require_documented_text("${level1_document}" "Level 1 contract"
  "bennu-v0.1.0-macos-arm64.tar.gz")
require_documented_text("${level1_document}" "Level 1 contract" "LICENSE")

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/documentation smoke")
set(repl_input "${work_directory}/repl-input.bennu")
set(emitted_c "${work_directory}/level1.c")
set(emitted_executable
  "${work_directory}/level1-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(native_executable
  "${work_directory}/level1-native${BENNU_EXECUTABLE_SUFFIX}")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")
configure_file("${example}" "${repl_input}" COPYONLY)

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" repl
  INPUT_FILE "${repl_input}"
  RESULT_VARIABLE repl_exit
  OUTPUT_VARIABLE repl_stdout
  ERROR_VARIABLE repl_stderr
)
string(REPLACE "\r\n" "\n" repl_stdout "${repl_stdout}")
if(NOT "${repl_exit}" STREQUAL "0" OR
   NOT repl_stdout STREQUAL "> >>(1 2 3 4 5)\n> >>6\n> " OR
   NOT repl_stderr STREQUAL "")
  message(FATAL_ERROR
    "documented REPL journey mismatch\nexit: ${repl_exit}\n"
    "stdout: [${repl_stdout}]\nstderr: [${repl_stderr}]")
endif()

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${example}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
if(NOT "${run_exit}" STREQUAL "0" OR
   NOT run_stdout STREQUAL expected_output OR NOT run_stderr STREQUAL "")
  message(FATAL_ERROR
    "documented run journey mismatch\nexit: ${run_exit}\n"
    "stdout: [${run_stdout}]\nstderr: [${run_stderr}]")
endif()

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${example}" -o "${emitted_c}"
  RESULT_VARIABLE emit_exit
  OUTPUT_VARIABLE emit_stdout
  ERROR_VARIABLE emit_stderr
)
if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
   NOT emit_stderr STREQUAL "" OR NOT EXISTS "${emitted_c}")
  message(FATAL_ERROR
    "documented emit-c journey mismatch\nexit: ${emit_exit}\n"
    "stdout: [${emit_stdout}]\nstderr: [${emit_stderr}]")
endif()

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 "${emitted_c}"
            "/Fe:${emitted_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE compile_exit
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
  )
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 "${emitted_c}" -o
            "${emitted_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE compile_exit
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
  )
endif()
if(NOT "${compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "documented emitted-C compile failed\nstdout: [${compile_stdout}]\n"
    "stderr: [${compile_stderr}]")
endif()

execute_process(
  COMMAND "${emitted_executable}"
  RESULT_VARIABLE emitted_exit
  OUTPUT_VARIABLE emitted_stdout
  ERROR_VARIABLE emitted_stderr
)
string(REPLACE "\r\n" "\n" emitted_stdout "${emitted_stdout}")
if(NOT "${emitted_exit}" STREQUAL "0" OR
   NOT emitted_stdout STREQUAL expected_output OR
   NOT emitted_stderr STREQUAL "")
  message(FATAL_ERROR
    "documented emitted-C executable mismatch\nexit: ${emitted_exit}\n"
    "stdout: [${emitted_stdout}]\nstderr: [${emitted_stderr}]")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=CC
          "${BENNU_EXECUTABLE}" build "${example}" -o "${native_executable}"
          --cc "${BENNU_C_COMPILER}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT "${build_exit}" STREQUAL "0" OR NOT build_stdout STREQUAL "" OR
   NOT build_stderr STREQUAL "" OR NOT EXISTS "${native_executable}")
  message(FATAL_ERROR
    "documented native-build journey mismatch\nexit: ${build_exit}\n"
    "stdout: [${build_stdout}]\nstderr: [${build_stderr}]")
endif()

execute_process(
  COMMAND "${native_executable}"
  RESULT_VARIABLE native_exit
  OUTPUT_VARIABLE native_stdout
  ERROR_VARIABLE native_stderr
)
string(REPLACE "\r\n" "\n" native_stdout "${native_stdout}")
if(NOT "${native_exit}" STREQUAL "0" OR
   NOT native_stdout STREQUAL expected_output OR NOT native_stderr STREQUAL "")
  message(FATAL_ERROR
    "documented native executable mismatch\nexit: ${native_exit}\n"
    "stdout: [${native_stdout}]\nstderr: [${native_stderr}]")
endif()

file(REMOVE_RECURSE "${work_directory}")
