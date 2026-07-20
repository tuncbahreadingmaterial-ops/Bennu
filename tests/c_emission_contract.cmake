foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/public typed C differential")
set(source_file "${work_directory}/rewrite-corpus.bennu")
set(generated_file "${work_directory}/rewrite-corpus.c")
set(repeated_file "${work_directory}/rewrite-corpus-repeat.c")
set(generated_executable
  "${work_directory}/rewrite-corpus${BENNU_EXECUTABLE_SUFFIX}")
set(direct_output "${work_directory}/direct.out")
set(generated_output "${work_directory}/generated.out")
set(repeated_output "${work_directory}/generated-repeat.out")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")
file(WRITE "${source_file}" [=[true
-9223372036854775808
-0.0
(false true false)
(7 -3 11 0)
(4.9406564584124654e-324 1.7976931348623157e308 inf -inf nan -0.0)
Bool()
Int()
Double()
inc 5
inc[1.5]
inc[(7 -3 11 0)]
add[1 2]
add[1 2.5]
add[10 (1 2 3)]
add[(1 2 3) 10]
add[(7 -3 11 0) (10 20 30 40)]
add[(1 2 3) 0.5]
equals[2 (1 2 3 2)]
equals[true (false true)]
not[(false true)]
add[Int() 0.5]
equals[Int() 10]
add[9007199254740993 0.0]
equals[9007199254740993 9007199254740992.0]
add[0.0 -0.0]
add[-0.0 -0.0]
add[1.7976931348623157e308 1.7976931348623157e308]
add[inf -inf]
equals[nan nan]
iota[3]
iota[-3]
inc iota 3
iota[4096]
]=])

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${source_file}"
  RESULT_VARIABLE direct_exit OUTPUT_FILE "${direct_output}"
  ERROR_VARIABLE direct_stderr)
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${generated_file}"
  RESULT_VARIABLE emit_exit OUTPUT_VARIABLE emit_stdout
  ERROR_VARIABLE emit_stderr)
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${repeated_file}"
  RESULT_VARIABLE repeat_exit OUTPUT_VARIABLE repeat_stdout
  ERROR_VARIABLE repeat_stderr)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
          "${generated_file}" "${repeated_file}"
  RESULT_VARIABLE deterministic_exit)
if(NOT "${direct_exit}" STREQUAL "0" OR NOT direct_stderr STREQUAL "" OR
   NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
   NOT emit_stderr STREQUAL "" OR NOT "${repeat_exit}" STREQUAL "0" OR
   NOT repeat_stdout STREQUAL "" OR NOT repeat_stderr STREQUAL "" OR
   NOT "${deterministic_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "public rewrite differential setup failed\n"
    "direct: ${direct_exit} [${direct_stderr}]\n"
    "emit: ${emit_exit} [${emit_stdout}] [${emit_stderr}]\n"
    "repeat: ${repeat_exit} [${repeat_stdout}] [${repeat_stderr}]")
endif()

file(READ "${generated_file}" emitted_source)
if(emitted_source MATCHES "${BENNU_SOURCE_DIR}" OR
   emitted_source MATCHES "${work_directory}" OR
   emitted_source MATCHES "[12][0-9][0-9][0-9]-[01][0-9]-[0-3][0-9]" OR
   NOT emitted_source MATCHES "BENNU_IMPL_INC_INT" OR
   NOT emitted_source MATCHES "BENNU_IMPL_ADD_DOUBLE" OR
   NOT emitted_source MATCHES "BENNU_IMPL_EQUALS_BOOL" OR
   NOT emitted_source MATCHES "BENNU_IMPL_NOT_BOOL" OR
   NOT emitted_source MATCHES "BENNU_IMPL_IOTA_INT" OR
   NOT emitted_source MATCHES
       "INT64_C\\(7\\), \\(-INT64_C\\(3\\)\\), INT64_C\\(11\\), INT64_C\\(0\\)" OR
   emitted_source MATCHES "for \\(int64_t value = INT64_C\\(1\\); value <= count")
  message(FATAL_ERROR
    "public emitter omitted typed lowering or retained count reconstruction")
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
    "strict public emitted C compilation failed\n"
    "stdout: [${compile_stdout}]\nstderr: [${compile_stderr}]")
endif()

execute_process(
  COMMAND "${generated_executable}"
  RESULT_VARIABLE generated_exit OUTPUT_FILE "${generated_output}"
  ERROR_VARIABLE generated_stderr)
execute_process(
  COMMAND "${generated_executable}"
  RESULT_VARIABLE repeated_run_exit OUTPUT_FILE "${repeated_output}"
  ERROR_VARIABLE repeated_run_stderr)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
          "${direct_output}" "${generated_output}"
  RESULT_VARIABLE compare_exit)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
          "${direct_output}" "${repeated_output}"
  RESULT_VARIABLE repeated_compare_exit)
if(NOT "${generated_exit}" STREQUAL "0" OR
   NOT "${repeated_run_exit}" STREQUAL "0" OR
   NOT generated_stderr STREQUAL "" OR NOT repeated_run_stderr STREQUAL "" OR
   NOT "${compare_exit}" STREQUAL "0" OR
   NOT "${repeated_compare_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "public emitted-C execution differs from the direct evaluator\n"
    "first: ${generated_exit} [${generated_stderr}] compare ${compare_exit}\n"
    "second: ${repeated_run_exit} [${repeated_run_stderr}] compare ${repeated_compare_exit}")
endif()

file(REMOVE_RECURSE "${work_directory}")
