if(NOT DEFINED BENNU_EXECUTABLE)
  message(FATAL_ERROR "BENNU_EXECUTABLE is required")
endif()

if(NOT DEFINED CASE)
  message(FATAL_ERROR "CASE is required")
endif()

if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

if(NOT DEFINED BENNU_C_COMPILER)
  message(FATAL_ERROR "BENNU_C_COMPILER is required")
endif()

if(NOT DEFINED BENNU_C_COMPILER_ID)
  message(FATAL_ERROR "BENNU_C_COMPILER_ID is required")
endif()

if(NOT DEFINED BENNU_EXECUTABLE_SUFFIX)
  message(FATAL_ERROR "BENNU_EXECUTABLE_SUFFIX is required")
endif()

function(check_run_error name source line column category message_text)
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}/bennu-${name}.bennu")
  file(WRITE "${source_file}" "${source}")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" run "${source_file}"
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
  )
  file(REMOVE "${source_file}")
  string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
  string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")
  set(expected_stderr
    "${source_file}:${line}:${column}: ${category}: ${message_text}\n")
  if("${actual_exit}" STREQUAL "0" OR NOT actual_stdout STREQUAL "" OR
     NOT actual_stderr STREQUAL expected_stderr)
    message(FATAL_ERROR
      "${name}: evaluator error process contract mismatch\n"
      "exit: ${actual_exit}\n"
      "stdout: [${actual_stdout}]\n"
      "expected stderr: [${expected_stderr}]\n"
      "actual stderr:   [${actual_stderr}]"
    )
  endif()
endfunction()

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
elseif(CASE STREQUAL "repl_transcript")
  set(arguments repl)
  set(input "ioata 5\ninc 5\n")
  set(expected_exit 0)
  set(expected_stdout "> >>(1 2 3 4 5)\n> >>6\n> ")
  set(expected_stderr "")
elseif(CASE STREQUAL "repl_eof")
  set(arguments repl)
  set(input "")
  set(expected_exit 0)
  set(expected_stdout "> ")
  set(expected_stderr "")
elseif(CASE STREQUAL "repl_recovers_after_errors")
  set(arguments repl)
  set(input [=[wat
inc 5
inc
inc 5
inc ioata 3
inc 5
inc 9223372036854775807
inc 5
]=])
  set(expected_exit 0)
  set(expected_stdout [=[> > >>6
> > >>6
> > >>6
> > >>6
> ]=])
  set(expected_stderr [=[<repl>:1:1: unknown name: unknown name: wat
<repl>:1:1: missing argument: primitive is missing its argument
<repl>:1:1: type mismatch: inc requires a scalar integer argument
<repl>:1:1: integer overflow: inc result exceeds the signed 64-bit range
]=])
elseif(CASE STREQUAL "repl_ignores_blank_lines")
  set(arguments repl)
  set(input "\n \t\ninc 5\n")
  set(expected_exit 0)
  set(expected_stdout "> > > >>6\n> ")
  set(expected_stderr "")
elseif(CASE STREQUAL "repl_rejects_arguments")
  set(arguments repl extra)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: 'repl' does not accept arguments\n")
elseif(CASE STREQUAL "run_example")
  set(arguments run "${BENNU_SOURCE_DIR}/tests/fixtures/level1-example.bennu")
  set(expected_exit 0)
  file(READ "${BENNU_SOURCE_DIR}/tests/fixtures/level1-example.out"
    expected_stdout)
  set(expected_stderr "")
elseif(CASE STREQUAL "run_path_with_spaces")
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}/level 1 example.bennu")
  set(source "ioata 5\ninc 5\n")
  set(arguments run "${source_file}")
  set(expected_exit 0)
  set(expected_stdout ">>(1 2 3 4 5)\n>>6\n")
  set(expected_stderr "")
elseif(CASE STREQUAL "run_crlf_and_blank_lines")
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}/bennu-${CASE}.bennu")
  if(WIN32)
    # file(WRITE) uses text mode and translates each LF to CRLF on Windows.
    set(source "\nioata 2\n \t\ninc 5\n")
  else()
    set(source "\r\nioata 2\r\n \t\r\ninc 5\r\n")
  endif()
  set(arguments run "${source_file}")
  set(expected_exit 0)
  set(expected_stdout ">>(1 2)\n>>6\n")
  set(expected_stderr "")
elseif(CASE STREQUAL "run_missing_path")
  set(arguments run)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: expected one source path after 'run'\n")
elseif(CASE STREQUAL "run_extra_argument")
  set(arguments run first.bennu second.bennu)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: 'run' expects exactly one source path\n")
elseif(CASE STREQUAL "run_nonexistent_file")
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.bennu")
  file(REMOVE "${source_file}")
  set(arguments run "${source_file}")
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr
    "${source_file}:1:1: file error: unable to read source\n")
elseif(CASE STREQUAL "run_directory_path")
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}")
  set(arguments run "${source_file}")
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr
    "${source_file}:1:1: file error: unable to read source\n")
elseif(CASE STREQUAL "run_unreadable_file")
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}/unreadable.bennu")
  set(source "inc 5\n")
  set(unreadable TRUE)
  set(arguments run "${source_file}")
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr
    "${source_file}:1:1: file error: unable to read source\n")
elseif(CASE STREQUAL "run_evaluator_errors")
  set(run_error_matrix TRUE)
elseif(CASE STREQUAL "run_batch_no_partial_output")
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}/bennu-${CASE}.bennu")
  set(source "inc 5\nwat\n")
  set(arguments run "${source_file}")
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr
    "${source_file}:2:1: unknown name: unknown name: wat\n")
elseif(CASE STREQUAL "emit_c_example")
  set(source_file "${BENNU_SOURCE_DIR}/examples/level1.bennu")
  set(output_file "${CMAKE_CURRENT_BINARY_DIR}/level 1 generated.c")
  set(output_executable
    "${CMAKE_CURRENT_BINARY_DIR}/level-1-generated${BENNU_EXECUTABLE_SUFFIX}")
  file(REMOVE "${output_file}" "${output_executable}")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${output_file}"
    RESULT_VARIABLE emit_exit
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
  )
  if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
     NOT emit_stderr STREQUAL "" OR NOT EXISTS "${output_file}")
    message(FATAL_ERROR
      "emit_c_example: emission failed\n"
      "exit: ${emit_exit}\nstdout: [${emit_stdout}]\nstderr: [${emit_stderr}]")
  endif()
  file(READ "${output_file}" emitted_source)
  file(READ "${BENNU_SOURCE_DIR}/tests/fixtures/level1-example.c"
    expected_emitted_source)
  if(NOT emitted_source STREQUAL expected_emitted_source OR
     emitted_source MATCHES "${BENNU_SOURCE_DIR}" OR
     emitted_source MATCHES "${source_file}")
    message(FATAL_ERROR
      "emit_c_example: generated C does not match the inspected golden C11")
  endif()
  if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
              "${output_file}" "/Fe:${output_executable}"
      RESULT_VARIABLE compile_exit
      OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr
    )
  else()
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
              "${output_file}" -o "${output_executable}"
      RESULT_VARIABLE compile_exit
      OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr
    )
  endif()
  if(NOT "${compile_exit}" STREQUAL "0")
    message(FATAL_ERROR
      "emit_c_example: generated C failed to compile\n"
      "stdout: [${compile_stdout}]\nstderr: [${compile_stderr}]")
  endif()
  execute_process(
    COMMAND "${output_executable}"
    RESULT_VARIABLE generated_exit
    OUTPUT_VARIABLE generated_stdout
    ERROR_VARIABLE generated_stderr
  )
  string(REPLACE "\r\n" "\n" generated_stdout "${generated_stdout}")
  if(NOT "${generated_exit}" STREQUAL "0" OR
     NOT generated_stdout STREQUAL ">>(1 2 3 4 5)\n>>6\n" OR
     NOT generated_stderr STREQUAL "")
    message(FATAL_ERROR
      "emit_c_example: generated executable contract mismatch\n"
      "exit: ${generated_exit}\n"
      "stdout: [${generated_stdout}]\nstderr: [${generated_stderr}]")
  endif()
  file(REMOVE "${output_file}" "${output_executable}")
  return()
elseif(CASE STREQUAL "emit_c_unwritable_atomic")
  set(source_file "${BENNU_SOURCE_DIR}/examples/level1.bennu")
  set(output_directory "${CMAKE_CURRENT_BINARY_DIR}/bennu-unwritable-output")
  set(output_file "${output_directory}/sentinel.c")
  file(REMOVE_RECURSE "${output_directory}")
  file(MAKE_DIRECTORY "${output_directory}")
  file(WRITE "${output_file}" "sentinel bytes\n")
  execute_process(COMMAND chmod 500 "${output_directory}")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${output_file}"
    RESULT_VARIABLE emit_exit
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
  )
  execute_process(COMMAND chmod 700 "${output_directory}")
  file(READ "${output_file}" preserved_output)
  file(GLOB orphan_outputs "${output_directory}/sentinel.c.tmp*")
  file(REMOVE_RECURSE "${output_directory}")
  if("${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
     NOT emit_stderr MATCHES "file error: unable to write output" OR
     NOT preserved_output STREQUAL "sentinel bytes\n" OR orphan_outputs)
    message(FATAL_ERROR
      "emit_c_unwritable_atomic: output was replaced or a partial was left\n"
      "exit: ${emit_exit}\nstdout: [${emit_stdout}]\nstderr: [${emit_stderr}]\n"
      "output: [${preserved_output}]\norphans: [${orphan_outputs}]")
  endif()
  return()
elseif(CASE STREQUAL "emit_c_empty_program")
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}/empty program.bennu")
  set(output_file "${CMAKE_CURRENT_BINARY_DIR}/empty program.c")
  set(output_executable
    "${CMAKE_CURRENT_BINARY_DIR}/empty-program${BENNU_EXECUTABLE_SUFFIX}")
  file(WRITE "${source_file}" " \t\n")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${output_file}"
    RESULT_VARIABLE emit_exit
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
  )
  if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
              "${output_file}" "/Fe:${output_executable}"
      RESULT_VARIABLE compile_exit
      OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr
    )
  else()
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
              "${output_file}" -o "${output_executable}"
      RESULT_VARIABLE compile_exit
      OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr
    )
  endif()
  if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
     NOT emit_stderr STREQUAL "" OR NOT "${compile_exit}" STREQUAL "0")
    message(FATAL_ERROR
      "emit_c_empty_program: empty program did not emit warning-clean C11\n"
      "emit exit: ${emit_exit}\nemit stdout: [${emit_stdout}]\n"
      "emit stderr: [${emit_stderr}]\ncompile stdout: [${compile_stdout}]\n"
      "compile stderr: [${compile_stderr}]")
  endif()
  execute_process(
    COMMAND "${output_executable}"
    RESULT_VARIABLE generated_exit
    OUTPUT_VARIABLE generated_stdout
    ERROR_VARIABLE generated_stderr
  )
  file(REMOVE "${source_file}" "${output_file}" "${output_executable}")
  if(NOT "${generated_exit}" STREQUAL "0" OR NOT generated_stdout STREQUAL "" OR
     NOT generated_stderr STREQUAL "")
    message(FATAL_ERROR "emit_c_empty_program: empty program emitted output")
  endif()
  return()
elseif(CASE STREQUAL "build_missing_source")
  set(arguments build)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr "error: expected a source path after 'build'\n")
elseif(CASE STREQUAL "build_invalid_arguments")
  set(arguments build sample.bennu --output native)
  set(expected_exit 1)
  set(expected_stdout "")
  set(expected_stderr
    "error: expected 'build <source> -o <output> [--cc <compiler>]'\n")
else()
  message(FATAL_ERROR "unknown test case: ${CASE}")
endif()

if(run_error_matrix)
  check_run_error(illegal_character "é" 1 1 "illegal character"
    "illegal character in source")
  check_run_error(malformed_integer "12x" 1 1 "malformed integer"
    "integer contains non-decimal characters")
  check_run_error(integer_out_of_range "9223372036854775808" 1 1
    "integer out of range" "integer literal is outside the signed 64-bit range")
  check_run_error(unknown_name "wat" 1 1 "unknown name" "unknown name: wat")
  check_run_error(missing_argument "inc" 1 1 "missing argument"
    "primitive is missing its argument")
  check_run_error(expected_whitespace "inc-5" 1 4 "expected whitespace"
    "primitive name and argument require whitespace")
  check_run_error(trailing_token "5 extra" 1 3 "trailing token"
    "expression has trailing input")
  check_run_error(integer_overflow "inc 9223372036854775807" 1 1
    "integer overflow" "inc result exceeds the signed 64-bit range")
  check_run_error(type_mismatch "inc ioata 3" 1 1 "type mismatch"
    "inc requires a scalar integer argument")
  check_run_error(allocation_limit "ioata 1000001" 1 1
    "allocation limit exceeded"
    "program ioata results exceed the Level 1 element limit")
  return()
endif()

if(DEFINED source)
  file(WRITE "${source_file}" "${source}")
  if(unreadable)
    execute_process(COMMAND chmod 000 "${source_file}")
  endif()
endif()

if(DEFINED input)
  set(input_file "${CMAKE_CURRENT_BINARY_DIR}/bennu-${CASE}.stdin")
  file(WRITE "${input_file}" "${input}")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" ${arguments}
    INPUT_FILE "${input_file}"
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
  )
  file(REMOVE "${input_file}")
else()
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" ${arguments}
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
  )
endif()

if(DEFINED source)
  if(unreadable)
    execute_process(COMMAND chmod 600 "${source_file}")
  endif()
  file(REMOVE "${source_file}")
endif()

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
