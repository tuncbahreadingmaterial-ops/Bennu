if(NOT DEFINED BENNU_EXECUTABLE)
  message(FATAL_ERROR "BENNU_EXECUTABLE is required")
endif()

if(NOT DEFINED CASE)
  message(FATAL_ERROR "CASE is required")
endif()

if(CASE STREQUAL "help")
  set(arguments --help)
  set(expected_exit 0)
  set(expected_stdout [=[Usage: bennu <command> [arguments]
       bennu --help

Commands:
  repl    Start an interactive Bennu session
  run     Run a Bennu source file
  emit-c  Emit C source for a Bennu source file
  build   Build a Bennu source file
]=])
  set(expected_stderr "")
elseif(CASE STREQUAL "no_arguments")
  set(arguments)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: expected a subcommand or --help\n")
elseif(CASE STREQUAL "unknown_option")
  set(arguments --wat)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: unknown option '--wat'\n")
elseif(CASE STREQUAL "unknown_subcommand")
  set(arguments wat)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: unknown subcommand 'wat'\n")
elseif(CASE STREQUAL "unimplemented_repl")
  set(arguments repl)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: subcommand 'repl' is not implemented\n")
elseif(CASE STREQUAL "unimplemented_run")
  set(arguments run sample.bn)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: subcommand 'run' is not implemented\n")
elseif(CASE STREQUAL "unimplemented_emit_c")
  set(arguments emit-c sample.bn)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: subcommand 'emit-c' is not implemented\n")
elseif(CASE STREQUAL "unimplemented_build")
  set(arguments build sample.bn)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: subcommand 'build' is not implemented\n")
else()
  message(FATAL_ERROR "unknown test case: ${CASE}")
endif()

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" ${arguments}
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE actual_stdout
  ERROR_VARIABLE actual_stderr
)

# Compare one CLI contract on every host despite Windows text-mode newlines.
string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")

if(NOT "${actual_exit}" STREQUAL "${expected_exit}")
  message(FATAL_ERROR
    "${CASE}: expected exit ${expected_exit}, got ${actual_exit}\n"
    "stdout: [${actual_stdout}]\n"
    "stderr: [${actual_stderr}]"
  )
endif()

if(NOT actual_stdout STREQUAL expected_stdout)
  message(FATAL_ERROR
    "${CASE}: stdout mismatch\n"
    "expected: [${expected_stdout}]\n"
    "actual:   [${actual_stdout}]"
  )
endif()

if(NOT actual_stderr STREQUAL expected_stderr)
  message(FATAL_ERROR
    "${CASE}: stderr mismatch\n"
    "expected: [${expected_stderr}]\n"
    "actual:   [${actual_stderr}]"
  )
endif()
