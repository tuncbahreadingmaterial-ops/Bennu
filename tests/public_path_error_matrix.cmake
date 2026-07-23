# TEST-ID: PUBLIC-ERROR-CLI-MATRIX
# TEST-ID: PARG-012-VALUE-INDEPENDENT-EMISSION
# TEST-ID: LOWERING-ARTIFACT-DYNAMIC-DIAGNOSTICS
# TEST-ID: PUBLIC-DYNAMIC-PRECEDENCE-PAIR-MATRIX
# TEST-ID: PUBLIC-EVALUATOR-GENERATED-NATIVE-DIAGNOSTIC-EQUIVALENCE
# TEST-ID: CHECKED-ARITHMETIC-PUBLIC-ERROR-MATRIX
foreach(required BENNU_EXECUTABLE BENNU_C_COMPILER BENNU_C_COMPILER_ID
                 BENNU_EXECUTABLE_SUFFIX)
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

function(check_public_dynamic_failure case_name source_text line column
         category message_text)
  set(source_file "${work_directory}/${case_name}.bennu")
  set(c_output "${work_directory}/${case_name}.c")
  set(emitted_output
    "${work_directory}/${case_name}-emitted${BENNU_EXECUTABLE_SUFFIX}")
  set(native_output
    "${work_directory}/${case_name}-native${BENNU_EXECUTABLE_SUFFIX}")
  set(expected
    "${source_file}:${line}:${column}: ${category}: ${message_text}\n")
  set(artifact_expected
    "bennu-source:${line}:${column}: ${category}: ${message_text}\n")
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
  file(READ "${c_output}" emitted_c)
  file(GLOB c_orphans "${c_output}.tmp*")
  if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
     NOT emit_stderr STREQUAL "" OR emitted_c STREQUAL "sentinel C bytes\n" OR
     c_orphans)
    message(FATAL_ERROR
      "PUBLIC-ERROR-MATRIX ${case_name} emit-c rejected a dynamic failure\n"
      "exit: ${emit_exit}\nstdout: [${emit_stdout}]\n"
      "stderr: [${emit_stderr}]\norphans: [${c_orphans}]")
  endif()

  if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /fp:strict /W4 /WX
              "${c_output}" "/Fe:${emitted_output}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr)
  else()
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" -std=c11 -frounding-math
              -ffp-contract=off -fno-fast-math -Wall -Wextra -Werror
              -pedantic-errors
              "${c_output}" -o "${emitted_output}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr)
  endif()
  if(NOT "${compile_exit}" STREQUAL "0")
    message(FATAL_ERROR
      "PUBLIC-ERROR-MATRIX ${case_name} strict C11 compilation failed\n"
      "stdout: [${compile_stdout}]\nstderr: [${compile_stderr}]")
  endif()
  execute_process(
    COMMAND "${emitted_output}"
    RESULT_VARIABLE emitted_exit OUTPUT_VARIABLE emitted_stdout
    ERROR_VARIABLE emitted_stderr)
  string(REPLACE "\r\n" "\n" emitted_stdout "${emitted_stdout}")
  string(REPLACE "\r\n" "\n" emitted_stderr "${emitted_stderr}")
  if("${emitted_exit}" STREQUAL "0" OR NOT emitted_stdout STREQUAL "" OR
     NOT emitted_stderr STREQUAL artifact_expected)
    message(FATAL_ERROR
      "PUBLIC-ERROR-MATRIX ${case_name} emitted runtime mismatch\n"
      "exit: ${emitted_exit}\nstdout: [${emitted_stdout}]\n"
      "expected stderr: [${artifact_expected}]\nactual stderr: [${emitted_stderr}]")
  endif()

  file(WRITE "${native_output}" "sentinel native bytes\n")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" build "${source_file}" -o "${native_output}"
            --cc "${BENNU_C_COMPILER}"
    RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
  if(NOT "${build_exit}" STREQUAL "0" OR NOT build_stdout STREQUAL "" OR
     NOT build_stderr STREQUAL "")
    message(FATAL_ERROR
      "PUBLIC-ERROR-MATRIX ${case_name} build rejected a dynamic failure\n"
      "exit: ${build_exit}\nstdout: [${build_stdout}]\n"
      "stderr: [${build_stderr}]")
  endif()
  execute_process(
    COMMAND "${native_output}"
    RESULT_VARIABLE native_exit OUTPUT_VARIABLE native_stdout
    ERROR_VARIABLE native_stderr)
  string(REPLACE "\r\n" "\n" native_stdout "${native_stdout}")
  string(REPLACE "\r\n" "\n" native_stderr "${native_stderr}")
  if("${native_exit}" STREQUAL "0" OR NOT native_stdout STREQUAL "" OR
     NOT native_stderr STREQUAL artifact_expected)
    message(FATAL_ERROR
      "PUBLIC-ERROR-MATRIX ${case_name} native runtime mismatch\n"
      "exit: ${native_exit}\nstdout: [${native_stdout}]\n"
      "expected stderr: [${artifact_expected}]\nactual stderr: [${native_stderr}]")
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
check_public_dynamic_failure(domain "inc 9223372036854775807" 1 1
  "DomainError" "inc failed: integer_overflow")
check_public_dynamic_failure(domain_vector
  "add[(0 9223372036854775807) (0 1)]" 1 1
  "DomainError" "add failed: integer_overflow at result index 1")
check_public_dynamic_failure(domain_dec "dec -9223372036854775808" 1 1
  "DomainError" "dec failed: integer_overflow")
check_public_dynamic_failure(domain_dec_vector
  "dec[(0 -9223372036854775808 -9223372036854775808)]" 1 1
  "DomainError" "dec failed: integer_overflow at result index 1")
check_public_dynamic_failure(domain_neg "neg -9223372036854775808" 1 1
  "DomainError" "neg failed: integer_overflow")
check_public_dynamic_failure(domain_neg_vector
  "neg[(0 -9223372036854775808 -9223372036854775808)]" 1 1
  "DomainError" "neg failed: integer_overflow at result index 1")
check_public_dynamic_failure(domain_abs "abs -9223372036854775808" 1 1
  "DomainError" "abs failed: integer_overflow")
check_public_dynamic_failure(domain_abs_vector
  "abs[(0 -9223372036854775808 -9223372036854775808)]" 1 1
  "DomainError" "abs failed: integer_overflow at result index 1")
check_public_dynamic_failure(domain_sub "sub[-9223372036854775808 1]" 1 1
  "DomainError" "sub failed: integer_overflow")
check_public_dynamic_failure(domain_sub_vector
  "sub[(0 -9223372036854775808 -9223372036854775808) (0 1 1)]" 1 1
  "DomainError" "sub failed: integer_overflow at result index 1")
check_public_dynamic_failure(domain_mul_vector
  "mul[(1 3037000500) (1 3037000500)]" 1 1
  "DomainError" "mul failed: integer_overflow at result index 1")
check_public_failure(shape_sub "sub[(1 2) (3)]" 1 11 "ShapeMismatch"
  "sub argument 2 expected shape [2], got [1]")
check_public_failure(shape_mul "mul[(1 2) (3)]" 1 11 "ShapeMismatch"
  "mul argument 2 expected shape [2], got [1]")
check_public_dynamic_failure(resource "iota[2305843009213693952]" 1 1
  "ResourceError" "iota resource request failed: size_overflow")
check_public_dynamic_failure(resource_child_before_shape
  "add[iota[2305843009213693952] (1 2)]" 1 5
  "ResourceError" "iota resource request failed: size_overflow")
check_public_dynamic_failure(domain_child_before_parent_shape
  "add[add[iota[3] (9223372036854775807 9223372036854775807 9223372036854775807)] (1 2)]"
  1 5 "DomainError" "add failed: integer_overflow at result index 0")
check_public_dynamic_failure(shape_before_same_call_domain
  "add[iota[3] (9223372036854775807 9223372036854775807)]"
  1 5 "ShapeMismatch" "add argument 1 expected shape [2], got [3]")
check_public_failure(static_shape_before_nested_domain
  "add[inc[(0 9223372036854775807)] (0)]" 1 34
  "ShapeMismatch" "add argument 2 expected shape [2], got [1]")
check_public_dynamic_failure(dynamic_shape_dynamic_dynamic
  "add[iota[2] iota[3]]" 1 13
  "ShapeMismatch" "add argument 2 expected shape [2], got [3]")
check_public_dynamic_failure(dynamic_shape_sub
  "sub[iota[2] iota[3]]" 1 13
  "ShapeMismatch" "sub argument 2 expected shape [2], got [3]")
check_public_dynamic_failure(dynamic_shape_mul
  "mul[iota[2] iota[3]]" 1 13
  "ShapeMismatch" "mul argument 2 expected shape [2], got [3]")
check_public_dynamic_failure(dynamic_shape_static_dynamic
  "add[(1 2) iota[3]]" 1 11
  "ShapeMismatch" "add argument 2 expected shape [2], got [3]")
check_public_dynamic_failure(dynamic_shape_dynamic_static
  "add[iota[3] (1 2)]" 1 5
  "ShapeMismatch" "add argument 1 expected shape [2], got [3]")
check_public_dynamic_failure(domain_root_before_later_shape
  "inc 9223372036854775807\nadd[iota[2] iota[3]]" 1 1
  "DomainError" "inc failed: integer_overflow")
check_public_dynamic_failure(shape_root_before_later_domain
  "add[iota[2] iota[3]]\ninc 9223372036854775807" 1 13
  "ShapeMismatch" "add argument 2 expected shape [2], got [3]")
check_public_dynamic_failure(resource_root_before_later_domain
  "iota[2305843009213693952]\ninc 9223372036854775807" 1 1
  "ResourceError" "iota resource request failed: size_overflow")
check_public_dynamic_failure(resource_root_before_later_shape
  "iota[2305843009213693952]\nadd[iota[2] iota[3]]" 1 1
  "ResourceError" "iota resource request failed: size_overflow")
check_public_dynamic_failure(domain_root_before_later_resource
  "inc 9223372036854775807\niota[2305843009213693952]" 1 1
  "DomainError" "inc failed: integer_overflow")
check_public_dynamic_failure(shape_root_before_later_resource
  "add[iota[2] iota[3]]\niota[2305843009213693952]" 1 13
  "ShapeMismatch" "add argument 2 expected shape [2], got [3]")
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
dec -9223372036854775808
inc 5
neg -9223372036854775808
inc 5
abs -9223372036854775808
inc 5
sub[-9223372036854775808 1]
inc 5
mul[-9223372036854775808 -1]
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
<repl>:1:1: DomainError: dec failed: integer_overflow
<repl>:1:1: DomainError: neg failed: integer_overflow
<repl>:1:1: DomainError: abs failed: integer_overflow
<repl>:1:1: DomainError: sub failed: integer_overflow
<repl>:1:1: DomainError: mul failed: integer_overflow
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
