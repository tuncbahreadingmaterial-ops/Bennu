# TEST-ID: PUBLIC-SUCCESS-CORPUS
# TEST-ID: PUBLIC-SUCCESS-CROSS-PATH-BYTES
# TEST-ID: PUBLIC-SUCCESS-NATIVE
foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/public path success matrix")
set(source_file
  "${BENNU_SOURCE_DIR}/tests/fixtures/public-path-success.bennu")
set(expected_file
  "${BENNU_SOURCE_DIR}/tests/fixtures/public-path-success.out")
set(generated_file "${work_directory}/public-path.c")
set(repeated_file "${work_directory}/public-path-repeat.c")
set(generated_executable
  "${work_directory}/public-path-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(native_executable
  "${work_directory}/public-path-native${BENNU_EXECUTABLE_SUFFIX}")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${source_file}"
  RESULT_VARIABLE runner_exit OUTPUT_VARIABLE runner_stdout
  ERROR_VARIABLE runner_stderr)
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" repl
  INPUT_FILE "${source_file}"
  RESULT_VARIABLE repl_exit OUTPUT_VARIABLE repl_stdout
  ERROR_VARIABLE repl_stderr)
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${generated_file}"
  RESULT_VARIABLE emit_exit OUTPUT_VARIABLE emit_stdout
  ERROR_VARIABLE emit_stderr)
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${repeated_file}"
  RESULT_VARIABLE repeat_emit_exit OUTPUT_VARIABLE repeat_emit_stdout
  ERROR_VARIABLE repeat_emit_stderr)
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" build "${source_file}" -o "${native_executable}"
          --cc "${BENNU_C_COMPILER}"
  RESULT_VARIABLE native_build_exit OUTPUT_VARIABLE native_build_stdout
  ERROR_VARIABLE native_build_stderr)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
          "${generated_file}" "${repeated_file}"
  RESULT_VARIABLE deterministic_exit)

if(NOT "${runner_exit}" STREQUAL "0" OR NOT runner_stderr STREQUAL "" OR
   NOT "${repl_exit}" STREQUAL "0" OR NOT repl_stderr STREQUAL "" OR
   NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
   NOT emit_stderr STREQUAL "" OR
   NOT "${repeat_emit_exit}" STREQUAL "0" OR
   NOT repeat_emit_stdout STREQUAL "" OR NOT repeat_emit_stderr STREQUAL "" OR
   NOT "${native_build_exit}" STREQUAL "0" OR
   NOT native_build_stdout STREQUAL "" OR NOT native_build_stderr STREQUAL "" OR
   NOT EXISTS "${native_executable}" OR
   NOT "${deterministic_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "PUBLIC-SUCCESS-MATRIX setup failed\n"
    "runner: ${runner_exit} [${runner_stderr}]\n"
    "repl: ${repl_exit} [${repl_stderr}]\n"
    "emit: ${emit_exit} [${emit_stdout}] [${emit_stderr}]\n"
    "repeat emit: ${repeat_emit_exit} [${repeat_emit_stdout}] [${repeat_emit_stderr}]\n"
    "native build: ${native_build_exit} [${native_build_stdout}] [${native_build_stderr}]")
endif()

file(READ "${generated_file}" emitted_source)
if(emitted_source MATCHES "${BENNU_SOURCE_DIR}" OR
   emitted_source MATCHES "${work_directory}" OR
   emitted_source MATCHES "[12][0-9][0-9][0-9]-[01][0-9]-[0-3][0-9]" OR
   NOT emitted_source MATCHES "BENNU_IMPL_INC_INT" OR
   NOT emitted_source MATCHES "BENNU_IMPL_INC_DOUBLE" OR
   NOT emitted_source MATCHES "BENNU_IMPL_ADD_INT" OR
   NOT emitted_source MATCHES "BENNU_IMPL_ADD_DOUBLE" OR
   NOT emitted_source MATCHES "BENNU_IMPL_EQUALS_BOOL" OR
   NOT emitted_source MATCHES "BENNU_IMPL_EQUALS_INT" OR
   NOT emitted_source MATCHES "BENNU_IMPL_EQUALS_DOUBLE" OR
   NOT emitted_source MATCHES "BENNU_IMPL_NOT_BOOL" OR
   NOT emitted_source MATCHES "BENNU_IMPL_IOTA_INT" OR
   NOT emitted_source MATCHES
       "INT64_C\\(7\\), \\(-INT64_C\\(3\\)\\), INT64_C\\(11\\), INT64_C\\(0\\)" OR
   emitted_source MATCHES "for \\(int64_t value = INT64_C\\(1\\); value <= count")
  message(FATAL_ERROR
    "PUBLIC-SUCCESS-MATRIX public emitter lowering contract failed")
endif()

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
            "${generated_file}" "/Fe:${generated_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
            "${generated_file}" -o "${generated_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)
endif()
if(NOT "${compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "PUBLIC-SUCCESS-MATRIX strict C11 compilation failed\n"
    "stdout: [${compile_stdout}]\nstderr: [${compile_stderr}]")
endif()

execute_process(
  COMMAND "${generated_executable}"
  RESULT_VARIABLE generated_exit OUTPUT_VARIABLE generated_stdout
  ERROR_VARIABLE generated_stderr)
execute_process(
  COMMAND "${generated_executable}"
  RESULT_VARIABLE generated_repeat_exit OUTPUT_VARIABLE generated_repeat_stdout
  ERROR_VARIABLE generated_repeat_stderr)
execute_process(
  COMMAND "${native_executable}"
  RESULT_VARIABLE native_exit OUTPUT_VARIABLE native_stdout
  ERROR_VARIABLE native_stderr)
execute_process(
  COMMAND "${native_executable}"
  RESULT_VARIABLE native_repeat_exit OUTPUT_VARIABLE native_repeat_stdout
  ERROR_VARIABLE native_repeat_stderr)
if(NOT "${generated_exit}" STREQUAL "0" OR
   NOT "${generated_repeat_exit}" STREQUAL "0" OR
   NOT "${native_exit}" STREQUAL "0" OR
   NOT "${native_repeat_exit}" STREQUAL "0" OR
   NOT generated_stderr STREQUAL "" OR
   NOT generated_repeat_stderr STREQUAL "" OR
   NOT native_stderr STREQUAL "" OR NOT native_repeat_stderr STREQUAL "")
  message(FATAL_ERROR
    "PUBLIC-SUCCESS-MATRIX emitted or native execution failed\n"
    "generated: ${generated_exit} [${generated_stderr}]\n"
    "generated repeat: ${generated_repeat_exit} [${generated_repeat_stderr}]\n"
    "native: ${native_exit} [${native_stderr}]\n"
    "native repeat: ${native_repeat_exit} [${native_repeat_stderr}]")
endif()

# Compare canonical LF bytes after removing only the REPL prompt protocol.
foreach(output runner_stdout repl_stdout generated_stdout generated_repeat_stdout
               native_stdout native_repeat_stdout)
  string(REPLACE "\r\n" "\n" ${output} "${${output}}")
endforeach()
string(REPLACE "> " "" repl_stdout "${repl_stdout}")
file(READ "${expected_file}" expected_stdout)
string(REPLACE "\r\n" "\n" expected_stdout "${expected_stdout}")
foreach(path_name runner repl emitted emitted-repeat native native-repeat)
  if(path_name STREQUAL "runner")
    set(actual "${runner_stdout}")
  elseif(path_name STREQUAL "repl")
    set(actual "${repl_stdout}")
  elseif(path_name STREQUAL "emitted")
    set(actual "${generated_stdout}")
  elseif(path_name STREQUAL "emitted-repeat")
    set(actual "${generated_repeat_stdout}")
  elseif(path_name STREQUAL "native")
    set(actual "${native_stdout}")
  else()
    set(actual "${native_repeat_stdout}")
  endif()
  file(WRITE "${work_directory}/${path_name}.normalized.out" "${actual}")
  if(NOT actual STREQUAL expected_stdout)
    message(FATAL_ERROR
      "PUBLIC-SUCCESS-MATRIX ${path_name} output differs from tracked corpus\n"
      "expected: [${expected_stdout}]\nactual: [${actual}]")
  endif()
endforeach()
foreach(path_name repl emitted emitted-repeat native native-repeat)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${work_directory}/runner.normalized.out"
            "${work_directory}/${path_name}.normalized.out"
    RESULT_VARIABLE comparison_exit)
  if(NOT "${comparison_exit}" STREQUAL "0")
    message(FATAL_ERROR
      "PUBLIC-SUCCESS-MATRIX runner and ${path_name} bytes differ")
  endif()
endforeach()

file(REMOVE_RECURSE "${work_directory}")
