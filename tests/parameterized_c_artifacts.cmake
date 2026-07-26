# TEST-ID: PARG-005-ARGUMENT-ERROR
# TEST-ID: PARG-006-TEXT-GRAMMAR
# TEST-ID: PARG-008-ZERO-ROOTS
# TEST-ID: PARG-009-DYNAMIC-IOTA-SHAPE
# TEST-ID: PARG-010-RUNTIME-ORDER
# TEST-ID: PARG-012-EMIT-C
# TEST-ID: PARG-013-NATIVE
# TEST-ID: PARG-014-DIAGNOSTICS
# TEST-ID: PARG-015-ATOMIC-STDOUT
# TEST-ID: PARG-016-REPRESENTABILITY
# TEST-ID: PARG-017-REGRESSION
# TEST-ID: PARG-018-PLATFORMS
foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory
  "${CMAKE_CURRENT_BINARY_DIR}/parameterized strict c artifacts")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")

function(normalize_output variable)
  string(REPLACE "\r\n" "\n" normalized "${${variable}}")
  set(${variable} "${normalized}" PARENT_SCOPE)
endfunction()

function(emit_compile_build stem source)
  set(generated "${work_directory}/${stem}.c")
  set(repeated "${work_directory}/${stem}-repeat.c")
  set(emitted_executable
    "${work_directory}/${stem}-emitted${BENNU_EXECUTABLE_SUFFIX}")
  set(native_executable
    "${work_directory}/${stem}-native${BENNU_EXECUTABLE_SUFFIX}")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source}" -o "${generated}"
    RESULT_VARIABLE emit_exit OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr)
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source}" -o "${repeated}"
    RESULT_VARIABLE repeat_exit OUTPUT_VARIABLE repeat_stdout
    ERROR_VARIABLE repeat_stderr)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${generated}" "${repeated}"
    RESULT_VARIABLE deterministic_exit)
  if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
              "${generated}" "/Fe:${emitted_executable}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr)
  else()
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic
              -Werror "${generated}" -o "${emitted_executable}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr)
  endif()
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" build "${source}" -o "${native_executable}"
            --cc "${BENNU_C_COMPILER}"
    RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
  if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
     NOT emit_stderr STREQUAL "" OR NOT "${repeat_exit}" STREQUAL "0" OR
     NOT repeat_stdout STREQUAL "" OR NOT repeat_stderr STREQUAL "" OR
     NOT "${deterministic_exit}" STREQUAL "0" OR
     NOT "${compile_exit}" STREQUAL "0" OR NOT "${build_exit}" STREQUAL "0" OR
     NOT build_stdout STREQUAL "" OR NOT build_stderr STREQUAL "")
    message(FATAL_ERROR
      "${stem}: emission/build setup failed\n"
      "emit=${emit_exit} [${emit_stdout}] [${emit_stderr}]\n"
      "repeat=${repeat_exit} [${repeat_stdout}] [${repeat_stderr}]\n"
      "deterministic=${deterministic_exit}\n"
      "compile=${compile_exit} [${compile_stdout}] [${compile_stderr}]\n"
      "build=${build_exit} [${build_stdout}] [${build_stderr}]")
  endif()
  set(${stem}_generated "${generated}" PARENT_SCOPE)
  set(${stem}_emitted "${emitted_executable}" PARENT_SCOPE)
  set(${stem}_native "${native_executable}" PARENT_SCOPE)
endfunction()

function(compile_custom_main stem generated custom_main)
  set(harness "${work_directory}/${stem}-custom-main.c")
  set(executable
    "${work_directory}/${stem}-custom-main${BENNU_EXECUTABLE_SUFFIX}")
  file(TO_CMAKE_PATH "${generated}" generated_include)
  file(WRITE "${harness}"
    "#define BENNU_CUSTOM_MAIN\n"
    "#include \"${generated_include}\"\n"
    "${custom_main}\n")
  if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
              "${harness}" "/Fe:${executable}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr)
  else()
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic
              -Werror "${harness}" -o "${executable}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr)
  endif()
  if(NOT "${compile_exit}" STREQUAL "0")
    message(FATAL_ERROR
      "${stem}: custom-main compilation failed\n"
      "compile=${compile_exit} [${compile_stdout}] [${compile_stderr}]")
  endif()
  set(${stem}_custom_main "${executable}" PARENT_SCOPE)
endfunction()

function(assert_paths_agree label source emitted native)
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" run "${source}" -- ${ARGN}
    RESULT_VARIABLE runner_exit OUTPUT_VARIABLE runner_stdout
    ERROR_VARIABLE runner_stderr)
  execute_process(
    COMMAND "${emitted}" ${ARGN}
    RESULT_VARIABLE emitted_exit OUTPUT_VARIABLE emitted_stdout
    ERROR_VARIABLE emitted_stderr)
  execute_process(
    COMMAND "${native}" ${ARGN}
    RESULT_VARIABLE native_exit OUTPUT_VARIABLE native_stdout
    ERROR_VARIABLE native_stderr)
  normalize_output(runner_stdout)
  normalize_output(runner_stderr)
  normalize_output(emitted_stdout)
  normalize_output(emitted_stderr)
  normalize_output(native_stdout)
  normalize_output(native_stderr)
  if(NOT "${runner_exit}" STREQUAL "${emitted_exit}" OR
     NOT "${runner_exit}" STREQUAL "${native_exit}" OR
     NOT runner_stdout STREQUAL emitted_stdout OR
     NOT runner_stdout STREQUAL native_stdout OR
     NOT runner_stderr STREQUAL emitted_stderr OR
     NOT runner_stderr STREQUAL native_stderr)
    message(FATAL_ERROR
      "${label}: runner/emitted/native disagreement\n"
      "runner=${runner_exit} [${runner_stdout}] [${runner_stderr}]\n"
      "emitted=${emitted_exit} [${emitted_stdout}] [${emitted_stderr}]\n"
      "native=${native_exit} [${native_stdout}] [${native_stderr}]")
  endif()
endfunction()

function(assert_paths_outcome label source emitted native expected_exit
         expected_stdout expected_stderr_pattern)
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" run "${source}" -- ${ARGN}
    RESULT_VARIABLE runner_exit OUTPUT_VARIABLE runner_stdout
    ERROR_VARIABLE runner_stderr)
  execute_process(
    COMMAND "${emitted}" ${ARGN}
    RESULT_VARIABLE emitted_exit OUTPUT_VARIABLE emitted_stdout
    ERROR_VARIABLE emitted_stderr)
  execute_process(
    COMMAND "${native}" ${ARGN}
    RESULT_VARIABLE native_exit OUTPUT_VARIABLE native_stdout
    ERROR_VARIABLE native_stderr)
  normalize_output(runner_stdout)
  normalize_output(runner_stderr)
  normalize_output(emitted_stdout)
  normalize_output(emitted_stderr)
  normalize_output(native_stdout)
  normalize_output(native_stderr)
  if(NOT "${runner_exit}" STREQUAL "${expected_exit}" OR
     NOT "${emitted_exit}" STREQUAL "${expected_exit}" OR
     NOT "${native_exit}" STREQUAL "${expected_exit}" OR
     NOT runner_stdout STREQUAL "${expected_stdout}" OR
     NOT emitted_stdout STREQUAL "${expected_stdout}" OR
     NOT native_stdout STREQUAL "${expected_stdout}" OR
     NOT runner_stderr STREQUAL emitted_stderr OR
     NOT runner_stderr STREQUAL native_stderr OR
     NOT runner_stderr MATCHES "${expected_stderr_pattern}")
    message(FATAL_ERROR
      "${label}: runner/emitted/native outcome disagreement\n"
      "expected=${expected_exit} [${expected_stdout}]"
      " [${expected_stderr_pattern}]\n"
      "runner=${runner_exit} [${runner_stdout}] [${runner_stderr}]\n"
      "emitted=${emitted_exit} [${emitted_stdout}] [${emitted_stderr}]\n"
      "native=${native_exit} [${native_stdout}] [${native_stderr}]")
  endif()
endfunction()

set(success_source
  "${BENNU_SOURCE_DIR}/tests/fixtures/parameterized-artifact-success.bennu")
set(double_source
  "${BENNU_SOURCE_DIR}/tests/fixtures/parameterized-artifact-double.bennu")
set(atomic_source
  "${BENNU_SOURCE_DIR}/tests/fixtures/parameterized-artifact-atomic.bennu")
set(literal_failure_source
  "${BENNU_SOURCE_DIR}/tests/fixtures/parameterized-artifact-literal-failure.bennu")
set(shape_source
  "${BENNU_SOURCE_DIR}/tests/fixtures/parameterized-artifact-shape.bennu")
set(zero_source
  "${BENNU_SOURCE_DIR}/tests/fixtures/parameterized-artifact-zero.bennu")
set(zero_roots_source
  "${BENNU_SOURCE_DIR}/tests/fixtures/parameterized-artifact-zero-roots.bennu")
set(absent_header_source
  "${BENNU_SOURCE_DIR}/tests/fixtures/parameterized-artifact-absent-header.bennu")

emit_compile_build(success "${success_source}")
emit_compile_build(double "${double_source}")
emit_compile_build(atomic "${atomic_source}")
emit_compile_build(literal_failure "${literal_failure_source}")
emit_compile_build(shape "${shape_source}")
emit_compile_build(zero "${zero_source}")
emit_compile_build(zero_roots "${zero_roots_source}")
emit_compile_build(absent_header "${absent_header_source}")

file(READ "${success_generated}" generated_source)
if(NOT generated_source MATCHES "int main\\(int argc, char \\*\\*argv\\)" OR
   NOT generated_source MATCHES "if \\(argc > 1\\)" OR
   NOT generated_source MATCHES
       "const uintmax_t bennu_host_count = \\(uintmax_t\\)\\(argc - 1\\)" OR
   NOT generated_source MATCHES
       "bennu_host_count > \\(uintmax_t\\)SIZE_MAX" OR
   NOT generated_source MATCHES
       "bennu_supplied = \\(size_t\\)bennu_host_count" OR
   NOT generated_source MATCHES
       "bennu_values\\[[0-9]+\\] = bennu_parameters\\[[0-9]+U\\]" OR
   NOT generated_source MATCHES "BENNU_IMPL_IOTA_INT" OR
   NOT generated_source MATCHES "BENNU_IMPL_ADD_DOUBLE" OR
   NOT generated_source MATCHES "BENNU_IMPL_NOT_BOOL" OR
   generated_source MATCHES "strtod_l|_strtod_l|newlocale|runtime overload")
  message(FATAL_ERROR
    "parameterized generated source violates the fixed-slot strict-C contract")
endif()

assert_paths_agree(valid-basic "${success_source}"
                   "${success_emitted}" "${success_native}"
                   3 0.5 true)
assert_paths_agree(valid-extrema "${success_source}"
                   "${success_emitted}" "${success_native}"
                   -9223372036854775808 -0.0 false)
assert_paths_agree(valid-infinity "${success_source}"
                   "${success_emitted}" "${success_native}"
                   0 inf true)

foreach(double_value IN ITEMS
    0.5
    -0.0
    2e0
    4.9406564584124654e-324
    5e-324
    2e-324
    -2e-324
    1.00000000000000011102230246251565404236316680908203125
    1.00000000000000011102230246251565404236316680908203126
    1.7976931348623157e308
    inf
    -inf
    nan)
  assert_paths_agree("double-${double_value}" "${double_source}"
                     "${double_emitted}" "${double_native}" "${double_value}")
endforeach()

assert_paths_agree(missing "${success_source}"
                   "${success_emitted}" "${success_native}" 3 0.5)
assert_paths_agree(extra "${success_source}"
                   "${success_emitted}" "${success_native}" 3 0.5 true extra)
assert_paths_agree(invalid-bool "${success_source}"
                   "${success_emitted}" "${success_native}" 3 0.5 TRUE)
assert_paths_agree(partial-int "${success_source}"
                   "${success_emitted}" "${success_native}" 3x 0.5 true)
assert_paths_agree(out-of-range-int "${success_source}"
                   "${success_emitted}" "${success_native}"
                   9223372036854775808 0.5 true)
assert_paths_agree(wrong-visible-double "${success_source}"
                   "${success_emitted}" "${success_native}" 3 2 true)
assert_paths_agree(hostile-double "${success_source}"
                   "${success_emitted}" "${success_native}" 3 "%n%n" true)
assert_paths_agree(out-of-range-double "${double_source}"
                   "${double_emitted}" "${double_native}"
                   1.7976931348623159e308)
assert_paths_agree(zero-success "${zero_source}"
                   "${zero_emitted}" "${zero_native}")
assert_paths_agree(zero-extra "${zero_source}"
                   "${zero_emitted}" "${zero_native}" unexpected)
assert_paths_outcome(zero-roots-success "${zero_roots_source}"
                     "${zero_roots_emitted}" "${zero_roots_native}"
                     0 "" "^$" 7 false)
assert_paths_outcome(zero-roots-missing "${zero_roots_source}"
                     "${zero_roots_emitted}" "${zero_roots_native}"
                     1 ""
                     "reason=missing.*required_count=2.*supplied_count=1.*position=2"
                     7)
assert_paths_outcome(zero-roots-malformed-first "${zero_roots_source}"
                     "${zero_roots_emitted}" "${zero_roots_native}"
                     1 ""
                     "reason=invalid_literal.*required_count=2.*supplied_count=2.*position=1"
                     7x false)
assert_paths_outcome(zero-roots-malformed-unused "${zero_roots_source}"
                     "${zero_roots_emitted}" "${zero_roots_native}"
                     1 ""
                     "reason=invalid_literal.*required_count=2.*supplied_count=2.*position=2"
                     7 FALSE)
assert_paths_outcome(absent-header-success "${absent_header_source}"
                     "${absent_header_emitted}" "${absent_header_native}"
                     0 "true\n" "^$")
assert_paths_outcome(absent-header-extra "${absent_header_source}"
                     "${absent_header_emitted}" "${absent_header_native}"
                     1 ""
                     "reason=extra.*required_count=0.*supplied_count=1.*position=1"
                     unexpected)

set(parameter_adapter_main [=[
#include <limits.h>
int main(void) {
  char *bennu_valid_argv[] = {
      (char *)"artifact", (char *)"3", (char *)"0.5", (char *)"true"};
  (void)bennu_execute;
  if (bennu_bind_arguments(0, NULL) != 0) { return 10; }
  if (bennu_bind_arguments(-1, NULL) != 0) { return 11; }
  if (bennu_bind_arguments(INT_MIN, NULL) != 0) { return 12; }
  if (bennu_bind_arguments(2, NULL) != 0) { return 13; }
  if (bennu_bind_arguments(INT_MAX, NULL) != 0) { return 14; }
  if (bennu_bind_arguments(4, bennu_valid_argv) == 0) { return 15; }
  return 0;
}
]=])
compile_custom_main(parameter_adapter "${success_generated}"
                    "${parameter_adapter_main}")
execute_process(
  COMMAND "${parameter_adapter_custom_main}"
  RESULT_VARIABLE parameter_adapter_exit
  OUTPUT_VARIABLE parameter_adapter_stdout
  ERROR_VARIABLE parameter_adapter_stderr)
normalize_output(parameter_adapter_stdout)
normalize_output(parameter_adapter_stderr)
string(REGEX MATCHALL "bennu_argument_error"
       parameter_adapter_records "${parameter_adapter_stderr}")
list(LENGTH parameter_adapter_records parameter_adapter_record_count)
if(NOT "${parameter_adapter_exit}" STREQUAL "0" OR
   NOT parameter_adapter_stdout STREQUAL "" OR
   NOT "${parameter_adapter_record_count}" STREQUAL "5" OR
   NOT parameter_adapter_stderr MATCHES
       "reason=missing.*required_count=3.*supplied_count=0.*position=1" OR
   NOT parameter_adapter_stderr MATCHES
       "reason=missing.*required_count=3.*supplied_count=1.*position=2" OR
   NOT parameter_adapter_stderr MATCHES
       "reason=extra.*required_count=3.*supplied_count=[1-9][0-9]+.*position=4")
  message(FATAL_ERROR
    "parameter adapter custom main failed\n"
    "exit=${parameter_adapter_exit} [${parameter_adapter_stdout}]"
    " [${parameter_adapter_stderr}]")
endif()

set(zero_adapter_main [=[
#include <limits.h>
int main(void) {
  (void)bennu_execute;
  if (bennu_bind_arguments(0, NULL) == 0) { return 20; }
  if (bennu_bind_arguments(-1, NULL) == 0) { return 21; }
  if (bennu_bind_arguments(INT_MIN, NULL) == 0) { return 22; }
  if (bennu_bind_arguments(1, NULL) == 0) { return 23; }
  if (bennu_bind_arguments(INT_MAX, NULL) != 0) { return 24; }
  return 0;
}
]=])
compile_custom_main(zero_adapter "${zero_generated}" "${zero_adapter_main}")
execute_process(
  COMMAND "${zero_adapter_custom_main}"
  RESULT_VARIABLE zero_adapter_exit OUTPUT_VARIABLE zero_adapter_stdout
  ERROR_VARIABLE zero_adapter_stderr)
normalize_output(zero_adapter_stdout)
normalize_output(zero_adapter_stderr)
string(REGEX MATCHALL "bennu_argument_error"
       zero_adapter_records "${zero_adapter_stderr}")
list(LENGTH zero_adapter_records zero_adapter_record_count)
if(NOT "${zero_adapter_exit}" STREQUAL "0" OR
   NOT zero_adapter_stdout STREQUAL "" OR
   NOT "${zero_adapter_record_count}" STREQUAL "1" OR
   NOT zero_adapter_stderr MATCHES
       "reason=extra.*required_count=0.*supplied_count=[1-9][0-9]+.*position=1")
  message(FATAL_ERROR
    "zero-parameter adapter custom main failed\n"
    "exit=${zero_adapter_exit} [${zero_adapter_stdout}]"
    " [${zero_adapter_stderr}]")
endif()

function(assert_dynamic_failure label source emitted native expected_pattern)
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" run "${source}" -- ${ARGN}
    RESULT_VARIABLE runner_exit OUTPUT_VARIABLE runner_stdout
    ERROR_VARIABLE runner_stderr)
  execute_process(
    COMMAND "${emitted}" ${ARGN}
    RESULT_VARIABLE emitted_exit OUTPUT_VARIABLE emitted_stdout
    ERROR_VARIABLE emitted_stderr)
  execute_process(
    COMMAND "${native}" ${ARGN}
    RESULT_VARIABLE native_exit OUTPUT_VARIABLE native_stdout
    ERROR_VARIABLE native_stderr)
  normalize_output(runner_stderr)
  normalize_output(emitted_stderr)
  normalize_output(native_stderr)
  if("${runner_exit}" STREQUAL "0" OR "${emitted_exit}" STREQUAL "0" OR
     "${native_exit}" STREQUAL "0" OR NOT runner_stdout STREQUAL "" OR
     NOT emitted_stdout STREQUAL "" OR NOT native_stdout STREQUAL "" OR
     NOT runner_stderr MATCHES "${expected_pattern}" OR
     NOT emitted_stderr MATCHES "${expected_pattern}" OR
     NOT native_stderr STREQUAL emitted_stderr)
    message(FATAL_ERROR
      "${label}: dynamic failure disagreement\n"
      "runner=${runner_exit} [${runner_stdout}] [${runner_stderr}]\n"
      "emitted=${emitted_exit} [${emitted_stdout}] [${emitted_stderr}]\n"
      "native=${native_exit} [${native_stdout}] [${native_stderr}]")
  endif()
endfunction()

assert_dynamic_failure(atomic "${atomic_source}"
                       "${atomic_emitted}" "${atomic_native}"
                       "DomainError: inc failed: integer_overflow"
                       9223372036854775807)
assert_dynamic_failure(literal-deferred "${literal_failure_source}"
                       "${literal_failure_emitted}" "${literal_failure_native}"
                       "DomainError: inc failed: integer_overflow" true)
assert_dynamic_failure(dynamic-shape "${shape_source}"
                       "${shape_emitted}" "${shape_native}"
                       "ShapeMismatch: add argument 1 expected shape"
                       3)

file(REMOVE_RECURSE "${work_directory}")
