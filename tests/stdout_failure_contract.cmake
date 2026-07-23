if(NOT DEFINED BENNU_EXECUTABLE)
  message(FATAL_ERROR "BENNU_EXECUTABLE is required")
endif()

if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

if(NOT EXISTS "/dev/full")
  message(FATAL_ERROR "/dev/full is required")
endif()

function(check_failure case_name)
  if(case_name STREQUAL "help")
    set(expected_stderr "error: unable to write stdout\n")
    execute_process(
      COMMAND "${BENNU_EXECUTABLE}" --help
      RESULT_VARIABLE actual_exit
      OUTPUT_FILE "/dev/full"
      ERROR_VARIABLE actual_stderr
    )
  elseif(case_name STREQUAL "run")
    set(expected_stderr
      "bennu_output_error reason=flush_failed pending_byte_count=67 accepted_byte_count=67 output_position=67\n")
    execute_process(
      COMMAND "${BENNU_EXECUTABLE}" run
              "${BENNU_SOURCE_DIR}/examples/rewrite.bennu"
      RESULT_VARIABLE actual_exit
      OUTPUT_FILE "/dev/full"
      ERROR_VARIABLE actual_stderr
    )
  elseif(case_name STREQUAL "repl")
    set(expected_stderr "error: unable to write stdout\n")
    set(input_file "${CMAKE_CURRENT_BINARY_DIR}/bennu-stdout-failure.stdin")
    file(WRITE "${input_file}" "inc 5\n")
    execute_process(
      COMMAND "${BENNU_EXECUTABLE}" repl
      INPUT_FILE "${input_file}"
      RESULT_VARIABLE actual_exit
      OUTPUT_FILE "/dev/full"
      ERROR_VARIABLE actual_stderr
    )
    file(REMOVE "${input_file}")
  else()
    message(FATAL_ERROR "unknown stdout failure case: ${case_name}")
  endif()

  if("${actual_exit}" STREQUAL "0" OR
     NOT actual_stderr STREQUAL expected_stderr)
    message(FATAL_ERROR
      "${case_name}: stdout failure contract mismatch\n"
      "exit: ${actual_exit}\n"
      "expected stderr: [${expected_stderr}]\n"
      "actual stderr:   [${actual_stderr}]"
    )
  endif()
endfunction()

check_failure(help)
check_failure(run)
check_failure(repl)
