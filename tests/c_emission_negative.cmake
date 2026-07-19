if(NOT DEFINED BENNU_EXECUTABLE)
  message(FATAL_ERROR "BENNU_EXECUTABLE is required")
endif()

function(expect_failure name expected_stderr)
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" ${ARGN}
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
  )
  string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
  string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")
  if("${actual_exit}" STREQUAL "0" OR NOT actual_stdout STREQUAL "" OR
     NOT actual_stderr STREQUAL expected_stderr)
    message(FATAL_ERROR
      "${name}: failure contract mismatch\nexit: ${actual_exit}\n"
      "stdout: [${actual_stdout}]\nexpected stderr: [${expected_stderr}]\n"
      "actual stderr: [${actual_stderr}]")
  endif()
endfunction()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/c emission negative")
set(valid_source "${work_directory}/valid source.bennu")
set(invalid_source "${work_directory}/invalid source.bennu")
set(overflow_source "${work_directory}/overflow source.bennu")
set(cumulative_source "${work_directory}/cumulative source.bennu")
set(output_file "${work_directory}/existing output.c")
set(missing_input "${work_directory}/missing source.bennu")
set(missing_parent_output "${work_directory}/missing parent/output.c")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")
file(WRITE "${valid_source}" "inc 5\n")
file(WRITE "${invalid_source}" "inc 5\nwat\n")
file(WRITE "${overflow_source}" "inc 9223372036854775807\n")
file(WRITE "${cumulative_source}" "ioata 600000\nioata 600000\n")

expect_failure(missing_source
  "error: expected a source path after 'emit-c'\n"
  emit-c)
expect_failure(missing_output_flag
  "error: expected 'emit-c <source> -o <output>'\n"
  emit-c "${valid_source}")
expect_failure(missing_output_path
  "error: expected 'emit-c <source> -o <output>'\n"
  emit-c "${valid_source}" -o)
expect_failure(wrong_output_flag
  "error: expected 'emit-c <source> -o <output>'\n"
  emit-c "${valid_source}" --output "${output_file}")
expect_failure(extra_argument
  "error: expected 'emit-c <source> -o <output>'\n"
  emit-c "${valid_source}" -o "${output_file}" extra)
expect_failure(nonexistent_input
  "${missing_input}:1:1: file error: unable to read source\n"
  emit-c "${missing_input}" -o "${output_file}")
expect_failure(directory_input
  "${work_directory}:1:1: file error: unable to read source\n"
  emit-c "${work_directory}" -o "${output_file}")

file(REMOVE "${output_file}")
expect_failure(invalid_source_new_output
  "${invalid_source}:2:1: unknown name: unknown name: wat\n"
  emit-c "${invalid_source}" -o "${output_file}")
if(EXISTS "${output_file}")
  message(FATAL_ERROR "invalid source left a new output file")
endif()

foreach(source_with_error "${invalid_source}" "${overflow_source}"
                          "${cumulative_source}")
  file(WRITE "${output_file}" "sentinel bytes\n")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_with_error}" -o "${output_file}"
    RESULT_VARIABLE error_exit
    OUTPUT_VARIABLE error_stdout
    ERROR_VARIABLE error_stderr
  )
  file(READ "${output_file}" preserved_output)
  file(GLOB orphan_outputs "${output_file}.tmp*")
  if("${error_exit}" STREQUAL "0" OR NOT error_stdout STREQUAL "" OR
     error_stderr STREQUAL "" OR NOT preserved_output STREQUAL "sentinel bytes\n" OR
     orphan_outputs)
    message(FATAL_ERROR
      "source error did not preserve output atomically: ${source_with_error}\n"
      "exit: ${error_exit}\nstdout: [${error_stdout}]\nstderr: [${error_stderr}]\n"
      "output: [${preserved_output}]\norphans: [${orphan_outputs}]")
  endif()
endforeach()

expect_failure(output_is_directory
  "${work_directory}:1:1: file error: unable to write output\n"
  emit-c "${valid_source}" -o "${work_directory}")
expect_failure(missing_output_parent
  "${missing_parent_output}:1:1: file error: unable to write output\n"
  emit-c "${valid_source}" -o "${missing_parent_output}")
file(GLOB output_failure_orphans "${work_directory}.tmp*" "${output_file}.tmp.*")
if(output_failure_orphans)
  message(FATAL_ERROR "output-path failure left temporary files: ${output_failure_orphans}")
endif()

file(WRITE "${output_file}.tmp" "occupied temporary candidate\n")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${valid_source}" -o "${output_file}"
  RESULT_VARIABLE collision_exit
  OUTPUT_VARIABLE collision_stdout
  ERROR_VARIABLE collision_stderr
)
file(READ "${output_file}.tmp" collision_contents)
file(GLOB collision_orphans "${output_file}.tmp.*")
if(NOT "${collision_exit}" STREQUAL "0" OR NOT collision_stdout STREQUAL "" OR
   NOT collision_stderr STREQUAL "" OR NOT EXISTS "${output_file}" OR
   NOT collision_contents STREQUAL "occupied temporary candidate\n" OR
   collision_orphans)
  message(FATAL_ERROR
    "exclusive temporary collision handling failed\nexit: ${collision_exit}\n"
    "stdout: [${collision_stdout}]\nstderr: [${collision_stderr}]\n"
    "candidate: [${collision_contents}]\norphans: [${collision_orphans}]")
endif()

file(REMOVE_RECURSE "${work_directory}")
