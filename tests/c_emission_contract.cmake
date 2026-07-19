foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/c emission differential")
set(runtime_directory "${CMAKE_CURRENT_BINARY_DIR}/c emission runtime")
set(source_file "${work_directory}/all level 1 cases.bennu")
set(generated_file "${work_directory}/all level 1 cases.c")
set(repeated_file "${work_directory}/all level 1 cases repeated.c")
set(generated_executable
  "${runtime_directory}/level1-differential${BENNU_EXECUTABLE_SUFFIX}")
set(direct_output "${work_directory}/direct.out")
set(generated_output "${work_directory}/generated.out")
file(REMOVE_RECURSE "${work_directory}" "${runtime_directory}")
file(MAKE_DIRECTORY "${work_directory}" "${runtime_directory}")
file(WRITE "${source_file}" [=[0
-1
-9223372036854775808
9223372036854775807
inc -1
inc inc -3
ioata 0
ioata -2
ioata inc 999999
]=])

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${source_file}"
  RESULT_VARIABLE direct_exit
  OUTPUT_FILE "${direct_output}"
  ERROR_VARIABLE direct_stderr
)
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${generated_file}"
  RESULT_VARIABLE emit_exit
  OUTPUT_VARIABLE emit_stdout
  ERROR_VARIABLE emit_stderr
)
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${repeated_file}"
  RESULT_VARIABLE repeat_exit
  OUTPUT_VARIABLE repeat_stdout
  ERROR_VARIABLE repeat_stderr
)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${generated_file}" "${repeated_file}"
  RESULT_VARIABLE deterministic_exit
)
if(NOT "${direct_exit}" STREQUAL "0" OR NOT direct_stderr STREQUAL "" OR
   NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
   NOT emit_stderr STREQUAL "" OR NOT "${repeat_exit}" STREQUAL "0" OR
   NOT repeat_stdout STREQUAL "" OR NOT repeat_stderr STREQUAL "" OR
   NOT "${deterministic_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "differential setup or deterministic emission failed\n"
    "direct: ${direct_exit} [${direct_stderr}]\n"
    "emit: ${emit_exit} [${emit_stdout}] [${emit_stderr}]\n"
    "repeat: ${repeat_exit} [${repeat_stdout}] [${repeat_stderr}]\n"
    "compare: ${deterministic_exit}")
endif()

file(READ "${generated_file}" emitted_source)
file(SIZE "${generated_file}" emitted_size)
if(emitted_size GREATER 10000 OR emitted_source MATCHES "${BENNU_SOURCE_DIR}" OR
   emitted_source MATCHES "${work_directory}" OR
   emitted_source MATCHES "[12][0-9][0-9][0-9]-[01][0-9]-[0-3][0-9]" OR
   NOT emitted_source MATCHES "INT64_C\\(9223372036854775807\\)" OR
   NOT emitted_source MATCHES "-INT64_C\\(9223372036854775807\\) - INT64_C\\(1\\)")
  message(FATAL_ERROR
    "generated source is host-dependent, unbounded, or missing signed boundaries")
endif()

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
            "${generated_file}" "/Fe:${generated_executable}"
    WORKING_DIRECTORY "${runtime_directory}"
    RESULT_VARIABLE compile_exit
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
  )
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
            "${generated_file}" -o "${generated_executable}"
    WORKING_DIRECTORY "${runtime_directory}"
    RESULT_VARIABLE compile_exit
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
  )
endif()
if(NOT "${compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "generated differential C failed to compile\n"
    "stdout: [${compile_stdout}]\nstderr: [${compile_stderr}]")
endif()

file(REMOVE "${source_file}" "${generated_file}" "${repeated_file}")
execute_process(
  COMMAND "${generated_executable}"
  WORKING_DIRECTORY "${runtime_directory}"
  RESULT_VARIABLE generated_exit
  OUTPUT_FILE "${generated_output}"
  ERROR_VARIABLE generated_stderr
)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${direct_output}" "${generated_output}"
  RESULT_VARIABLE output_compare_exit
)
file(SIZE "${generated_output}" generated_output_size)
file(REMOVE_RECURSE "${work_directory}" "${runtime_directory}")
if(NOT "${generated_exit}" STREQUAL "0" OR NOT generated_stderr STREQUAL "" OR
   NOT "${output_compare_exit}" STREQUAL "0" OR
   generated_output_size LESS 6888900)
  message(FATAL_ERROR
    "compiled C differs from direct Level 1 evaluation\n"
    "exit: ${generated_exit}\nstderr: [${generated_stderr}]\n"
    "compare: ${output_compare_exit}\nbytes: ${generated_output_size}")
endif()
