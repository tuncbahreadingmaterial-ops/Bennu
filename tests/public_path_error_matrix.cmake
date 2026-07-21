# TEST-ID: PUBLIC-ERROR-CLI-MATRIX
foreach(required BENNU_EXECUTABLE BENNU_C_COMPILER BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/public path error matrix")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")

function(check_public_failure case_name source_text line column category message_text)
  set(source_file "${work_directory}/${case_name}.bennu")
  set(c_output "${work_directory}/${case_name}.c")
  set(native_output
    "${work_directory}/${case_name}-native${BENNU_EXECUTABLE_SUFFIX}")
  set(expected
    "${source_file}:${line}:${column}: ${category}: ${message_text}\n")
  file(WRITE "${source_file}" "${source_text}\n")

  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" run "${source_file}"
    RESULT_VARIABLE runner_exit OUTPUT_VARIABLE runner_stdout
    ERROR_VARIABLE runner_stderr)
  string(REPLACE "\r\n" "\n" runner_stdout "${runner_stdout}")
  string(REPLACE "\r\n" "\n" runner_stderr "${runner_stderr}")
  if("${runner_exit}" STREQUAL "0" OR NOT runner_stdout STREQUAL "" OR
     NOT runner_stderr STREQUAL expected)
    message(FATAL_ERROR
      "PUBLIC-ERROR-MATRIX ${case_name} runner failure mismatch\n"
      "exit: ${runner_exit}\nstdout: [${runner_stdout}]\n"
      "expected stderr: [${expected}]\nactual stderr: [${runner_stderr}]")
  endif()

  file(WRITE "${c_output}" "sentinel C bytes\n")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_file}" -o "${c_output}"
    RESULT_VARIABLE emit_exit OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr)
  string(REPLACE "\r\n" "\n" emit_stdout "${emit_stdout}")
  string(REPLACE "\r\n" "\n" emit_stderr "${emit_stderr}")
  file(READ "${c_output}" preserved_c)
  file(GLOB c_orphans "${c_output}.tmp*")
  if("${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
     NOT emit_stderr STREQUAL expected OR
     NOT preserved_c STREQUAL "sentinel C bytes\n" OR c_orphans)
    message(FATAL_ERROR
      "PUBLIC-ERROR-MATRIX ${case_name} emit-c was not exact and atomic\n"
      "exit: ${emit_exit}\nstdout: [${emit_stdout}]\n"
      "expected stderr: [${expected}]\nactual stderr: [${emit_stderr}]\n"
      "output: [${preserved_c}]\norphans: [${c_orphans}]")
  endif()

  file(WRITE "${native_output}" "sentinel native bytes\n")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" build "${source_file}" -o "${native_output}"
            --cc "${BENNU_C_COMPILER}"
    RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
  string(REPLACE "\r\n" "\n" build_stdout "${build_stdout}")
  string(REPLACE "\r\n" "\n" build_stderr "${build_stderr}")
  file(READ "${native_output}" preserved_native)
  file(GLOB native_orphans "${native_output}.tmp*")
  if("${build_exit}" STREQUAL "0" OR NOT build_stdout STREQUAL "" OR
     NOT build_stderr STREQUAL expected OR
     NOT preserved_native STREQUAL "sentinel native bytes\n" OR native_orphans)
    message(FATAL_ERROR
      "PUBLIC-ERROR-MATRIX ${case_name} native rejection was not exact and atomic\n"
      "exit: ${build_exit}\nstdout: [${build_stdout}]\n"
      "expected stderr: [${expected}]\nactual stderr: [${build_stderr}]\n"
      "output: [${preserved_native}]\norphans: [${native_orphans}]")
  endif()
endfunction()

check_public_failure(invalid_byte "é" 1 1 "InvalidByte"
  "invalid source byte")
check_public_failure(malformed_literal "12x" 1 1 "MalformedLiteral"
  "malformed scalar literal")
check_public_failure(literal_range "9223372036854775808" 1 1
  "LiteralRangeError" "scalar literal is outside its accepted range")
check_public_failure(syntax "inc " 1 5 "SyntaxError"
  "expected an expression")
check_public_failure(unknown "wat[1]" 1 1 "UnknownPrimitive"
  "unknown primitive 'wat'")
check_public_failure(arity "add[1]" 1 1 "ArityError"
  "add received 1 argument(s); accepted arity 2")
check_public_failure(type "add[1 true]" 1 7 "TypeError"
  "add arguments do not match an accepted signature; first unsupported argument is 2")
check_public_failure(shape "add[(1 2) (3)]" 1 11 "ShapeMismatch"
  "add argument 2 expected shape [2], got [1]")
check_public_failure(domain "inc 9223372036854775807" 1 1 "DomainError"
  "inc failed: integer_overflow")
check_public_failure(resource "iota[2305843009213693952]" 1 1 "ResourceError"
  "iota resource request failed: size_overflow")
check_public_failure(late_transaction "inc 5\nwat[1]" 2 1 "UnknownPrimitive"
  "unknown primitive 'wat'")

set(repl_input "${work_directory}/matrix.stdin")
file(WRITE "${repl_input}" [=[é
inc 5
12x
inc 5
9223372036854775808
inc 5
inc[
inc 5
wat[1]
inc 5
add[1]
inc 5
add[1 true]
inc 5
add[(1 2) (3)]
inc 5
inc 9223372036854775807
inc 5
iota[2305843009213693952]
inc 5
equals[2 2]
not[false]
Bool()
Int()
Double()
]=])
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" repl
  INPUT_FILE "${repl_input}"
  RESULT_VARIABLE repl_exit OUTPUT_VARIABLE repl_stdout
  ERROR_VARIABLE repl_stderr)
string(REPLACE "\r\n" "\n" repl_stdout "${repl_stdout}")
string(REPLACE "\r\n" "\n" repl_stderr "${repl_stderr}")
set(expected_repl_stdout [=[> > 6
> > 6
> > 6
> > 6
> > 6
> > 6
> > 6
> > 6
> > 6
> > 6
> true
> true
> ()
> ()
> ()
> ]=])
set(expected_repl_stderr [=[<repl>:1:1: InvalidByte: invalid source byte
<repl>:1:1: MalformedLiteral: malformed scalar literal
<repl>:1:1: LiteralRangeError: scalar literal is outside its accepted range
<repl>:1:5: SyntaxError: missing closing delimiter
<repl>:1:1: UnknownPrimitive: unknown primitive 'wat'
<repl>:1:1: ArityError: add received 1 argument(s); accepted arity 2
<repl>:1:7: TypeError: add arguments do not match an accepted signature; first unsupported argument is 2
<repl>:1:11: ShapeMismatch: add argument 2 expected shape [2], got [1]
<repl>:1:1: DomainError: inc failed: integer_overflow
<repl>:1:1: ResourceError: iota resource request failed: size_overflow
]=])
if(NOT "${repl_exit}" STREQUAL "0" OR
   NOT repl_stdout STREQUAL expected_repl_stdout OR
   NOT repl_stderr STREQUAL expected_repl_stderr)
  message(FATAL_ERROR
    "PUBLIC-ERROR-MATRIX REPL category, recovery, or reset mismatch\n"
    "exit: ${repl_exit}\nexpected stdout: [${expected_repl_stdout}]\n"
    "actual stdout: [${repl_stdout}]\n"
    "expected stderr: [${expected_repl_stderr}]\n"
    "actual stderr: [${repl_stderr}]")
endif()

file(REMOVE_RECURSE "${work_directory}")
