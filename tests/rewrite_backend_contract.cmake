foreach(required BENNU_REWRITE_BACKEND_DRIVER BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/rewrite backend typed smoke")
set(source_file "${work_directory}/arbitrary-vector.bennu")
set(c_file "${work_directory}/arbitrary-vector.c")
set(executable "${work_directory}/arbitrary-vector${BENNU_EXECUTABLE_SUFFIX}")
set(oracle_file "${work_directory}/oracle.out")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")
file(WRITE "${source_file}" "inc[(7 -3 11 0)]\n")

execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" oracle "${source_file}" "${oracle_file}"
  RESULT_VARIABLE oracle_exit
  OUTPUT_VARIABLE oracle_stdout
  ERROR_VARIABLE oracle_stderr
)
if(NOT "${oracle_exit}" STREQUAL "0" OR NOT oracle_stdout STREQUAL "" OR
   NOT oracle_stderr STREQUAL "")
  message(FATAL_ERROR "rewrite evaluator oracle failed")
endif()

execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" emit "${source_file}" "${c_file}"
  RESULT_VARIABLE emit_exit
  OUTPUT_VARIABLE emit_stdout
  ERROR_VARIABLE emit_stderr
)
if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
   NOT emit_stderr STREQUAL "")
  message(FATAL_ERROR
    "typed rewrite emission failed\nexit: ${emit_exit}\n"
    "stdout: [${emit_stdout}]\nstderr: [${emit_stderr}]")
endif()

file(READ "${c_file}" emitted_source)
if(emitted_source MATCHES "\\(8 -2 12 1\\)" OR
   NOT emitted_source MATCHES "for \\(" OR
   NOT emitted_source MATCHES "BENNU_IMPL_INC_INT")
  message(FATAL_ERROR "generated C did not lower the typed vector operation")
endif()

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
            "${c_file}" "/Fe:${executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE compile_exit
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
  )
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
            "${c_file}" -o "${executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE compile_exit
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
  )
endif()
if(NOT "${compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "strict generated-C compilation failed\n"
    "stdout: [${compile_stdout}]\nstderr: [${compile_stderr}]")
endif()

execute_process(
  COMMAND "${executable}"
  WORKING_DIRECTORY "${work_directory}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
file(READ "${oracle_file}" oracle_output)
if(NOT "${run_exit}" STREQUAL "0" OR
   NOT run_stdout STREQUAL oracle_output OR
   NOT oracle_output STREQUAL "(8 -2 12 1)\n" OR
   NOT run_stderr STREQUAL "")
  message(FATAL_ERROR
    "typed generated runtime mismatch\nexit: ${run_exit}\n"
    "stdout: [${run_stdout}]\nstderr: [${run_stderr}]")
endif()

set(corpus_source "${work_directory}/differential-corpus.bennu")
set(corpus_oracle "${work_directory}/differential-corpus.oracle")
set(corpus_c "${work_directory}/differential-corpus.c")
set(corpus_repeat_c "${work_directory}/differential-corpus-repeat.c")
set(corpus_executable
  "${work_directory}/differential-corpus${BENNU_EXECUTABLE_SUFFIX}")
set(corpus_native
  "${work_directory}/differential-native${BENNU_EXECUTABLE_SUFFIX}")
file(WRITE "${corpus_source}" [=[true
-9223372036854775808
-0.0
100000.0
(false true false)
(7 -3 11 0)
(4.9406564584124654e-324 2.225073858507201e-308 2.2250738585072014e-308 1.7976931348623157e308 inf -inf nan -0.0)
Bool()
Int()
Double()
inc 5
inc inc 5
inc[1.5]
inc[-1.0]
inc[-0.0]
inc[(4.9406564584124654e-324 1.7976931348623157e308 inf -inf nan)]
inc[(7 -3 11 0)]
add[1 2]
add[1 2.5]
add[10 (1 2 3)]
add[(1 2 3) 10]
add[(1 2 3) (10 20 30)]
add[(1 2 3) 0.5]
add[0.5 (1 2 3)]
equals[2 (1 2 3 2)]
equals[(1 2 3 2) 2]
equals[true (false true)]
not[(false true)]
add[Int() 0.5]
equals[Int() 10]
add[9007199254740993 0.0]
add[-9007199254740993 0.0]
equals[9007199254740993 9007199254740992.0]
equals[-9007199254740993 -9007199254740992.0]
add[0.0 -0.0]
add[-0.0 -0.0]
add[1.0 1.1102230246251565e-16]
add[1.0 1.6653345369377348e-16]
add[2.2250738585072014e-308 -2.225073858507201e-308]
add[1.7976931348623157e308 1.7976931348623157e308]
add[-1.7976931348623157e308 -1.7976931348623157e308]
add[4.9406564584124654e-324 4.9406564584124654e-324]
add[inf -inf]
equals[nan nan]
equals[inf inf]
equals[inf -inf]
iota[3]
iota[-3]
inc[iota[3]]
iota[0]
inc[inc[iota[7]]]
iota[4096]
]=])

execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" oracle "${corpus_source}" "${corpus_oracle}"
  RESULT_VARIABLE corpus_oracle_exit
  OUTPUT_VARIABLE corpus_oracle_stdout
  ERROR_VARIABLE corpus_oracle_stderr)
execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" emit "${corpus_source}" "${corpus_c}"
  RESULT_VARIABLE corpus_emit_exit
  OUTPUT_VARIABLE corpus_emit_stdout
  ERROR_VARIABLE corpus_emit_stderr)
execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" emit "${corpus_source}" "${corpus_repeat_c}"
  RESULT_VARIABLE corpus_repeat_exit
  OUTPUT_VARIABLE corpus_repeat_stdout
  ERROR_VARIABLE corpus_repeat_stderr)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${corpus_c}" "${corpus_repeat_c}"
  RESULT_VARIABLE corpus_deterministic_exit)
if(NOT "${corpus_oracle_exit}" STREQUAL "0" OR
   NOT corpus_oracle_stdout STREQUAL "" OR NOT corpus_oracle_stderr STREQUAL "" OR
   NOT "${corpus_emit_exit}" STREQUAL "0" OR
   NOT corpus_emit_stdout STREQUAL "" OR NOT corpus_emit_stderr STREQUAL "" OR
   NOT "${corpus_repeat_exit}" STREQUAL "0" OR
   NOT corpus_repeat_stdout STREQUAL "" OR NOT corpus_repeat_stderr STREQUAL "" OR
   NOT "${corpus_deterministic_exit}" STREQUAL "0")
  message(FATAL_ERROR "rewrite differential corpus setup failed")
endif()
file(READ "${corpus_c}" corpus_emitted_source)
if(corpus_emitted_source MATCHES "${work_directory}" OR
   corpus_emitted_source MATCHES "${corpus_source}" OR
   corpus_emitted_source MATCHES "[12][0-9][0-9][0-9]-[01][0-9]-[0-3][0-9]" OR
   NOT corpus_emitted_source MATCHES "BENNU_IMPL_ADD_DOUBLE" OR
   NOT corpus_emitted_source MATCHES "BENNU_IMPL_EQUALS_BOOL" OR
   NOT corpus_emitted_source MATCHES "BENNU_IMPL_IOTA_INT")
  message(FATAL_ERROR "generated corpus C is incomplete or nondeterministic")
endif()
if(NOT corpus_emitted_source MATCHES
       "static BennuValue bennu_values\\[")
  message(FATAL_ERROR "generated value metadata uses the bounded call stack")
endif()

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
            "${corpus_c}" "/Fe:${corpus_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE corpus_compile_exit
    OUTPUT_VARIABLE corpus_compile_stdout
    ERROR_VARIABLE corpus_compile_stderr)
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
            "${corpus_c}" -o "${corpus_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE corpus_compile_exit
    OUTPUT_VARIABLE corpus_compile_stdout
    ERROR_VARIABLE corpus_compile_stderr)
endif()
if(NOT "${corpus_compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "strict differential C compilation failed\n${corpus_compile_stdout}\n${corpus_compile_stderr}")
endif()
execute_process(
  COMMAND "${corpus_executable}"
  WORKING_DIRECTORY "${work_directory}"
  RESULT_VARIABLE corpus_run_exit
  OUTPUT_FILE "${work_directory}/differential-corpus.generated"
  ERROR_VARIABLE corpus_run_stderr)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
          "${corpus_oracle}" "${work_directory}/differential-corpus.generated"
  RESULT_VARIABLE corpus_compare_exit)
if(NOT "${corpus_run_exit}" STREQUAL "0" OR
   NOT corpus_run_stderr STREQUAL "" OR NOT "${corpus_compare_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "emitted-C differential corpus mismatch\nexit: ${corpus_run_exit}\n"
    "stderr: [${corpus_run_stderr}]\ncompare: ${corpus_compare_exit}")
endif()

execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" build "${corpus_source}"
          "${corpus_native}" "${BENNU_C_COMPILER}"
  RESULT_VARIABLE native_build_exit
  OUTPUT_VARIABLE native_build_stdout
  ERROR_VARIABLE native_build_stderr)
if(NOT "${native_build_exit}" STREQUAL "0" OR
   NOT native_build_stdout STREQUAL "" OR NOT native_build_stderr STREQUAL "" OR
   NOT EXISTS "${corpus_native}")
  message(FATAL_ERROR
    "internal rewrite native build failed\nexit: ${native_build_exit}\n"
    "stdout: [${native_build_stdout}]\nstderr: [${native_build_stderr}]")
endif()
execute_process(
  COMMAND "${corpus_native}"
  WORKING_DIRECTORY "${work_directory}"
  RESULT_VARIABLE native_run_exit
  OUTPUT_FILE "${work_directory}/differential-native.out"
  ERROR_VARIABLE native_run_stderr)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
          "${corpus_oracle}" "${work_directory}/differential-native.out"
  RESULT_VARIABLE native_compare_exit)
if(NOT "${native_run_exit}" STREQUAL "0" OR
   NOT native_run_stderr STREQUAL "" OR NOT "${native_compare_exit}" STREQUAL "0")
  message(FATAL_ERROR "native rewrite differential corpus mismatch")
endif()

function(expect_emission_failure case_name mode source_text)
  set(case_source "${work_directory}/${case_name}.bennu")
  set(case_output "${work_directory}/${case_name}.c")
  set(case_oracle "${work_directory}/${case_name}.out")
  set(case_native "${work_directory}/${case_name}${BENNU_EXECUTABLE_SUFFIX}")
  string(REPLACE "emit" "oracle" oracle_mode "${mode}")
  string(REPLACE "emit" "build" build_mode "${mode}")
  file(WRITE "${case_source}" "${source_text}")
  file(REMOVE "${case_output}" "${case_oracle}")
  file(WRITE "${case_native}" "sentinel native bytes")
  execute_process(
    COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" "${oracle_mode}"
            "${case_source}" "${case_oracle}"
    RESULT_VARIABLE oracle_case_exit
    OUTPUT_VARIABLE oracle_case_stdout
    ERROR_VARIABLE oracle_case_stderr)
  execute_process(
    COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" "${mode}" "${case_source}" "${case_output}"
    RESULT_VARIABLE case_exit
    OUTPUT_VARIABLE case_stdout
    ERROR_VARIABLE case_stderr)
  execute_process(
    COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" "${build_mode}"
            "${case_source}" "${case_native}" "${BENNU_C_COMPILER}"
    RESULT_VARIABLE build_case_exit
    OUTPUT_VARIABLE build_case_stdout
    ERROR_VARIABLE build_case_stderr)
  file(READ "${case_native}" preserved_native)
  if("${oracle_case_exit}" STREQUAL "0" OR
     NOT oracle_case_stdout STREQUAL "" OR oracle_case_stderr STREQUAL "" OR
     EXISTS "${case_oracle}" OR
     "${case_exit}" STREQUAL "0" OR NOT case_stdout STREQUAL "" OR
     case_stderr STREQUAL "" OR EXISTS "${case_output}")
    message(FATAL_ERROR
      "${case_name}: invalid rewrite published C bytes\nexit: ${case_exit}\n"
      "stdout: [${case_stdout}]\nstderr: [${case_stderr}]")
  endif()
  if("${build_case_exit}" STREQUAL "0" OR
     NOT build_case_stdout STREQUAL "" OR build_case_stderr STREQUAL "" OR
     NOT preserved_native STREQUAL "sentinel native bytes")
    message(FATAL_ERROR "${case_name}: native rejection changed output")
  endif()
endfunction()

expect_emission_failure(invalid_syntax emit "add[1, 2]\n")
expect_emission_failure(unknown_primitive emit "bogus[1]\n")
expect_emission_failure(invalid_arity emit "add[1]\n")
expect_emission_failure(invalid_type emit "add[1 true]\n")
expect_emission_failure(invalid_iota_container emit "iota[(1)]\n")
expect_emission_failure(invalid_shape emit "add[(1 2) (3)]\n")
expect_emission_failure(invalid_empty_shape emit "equals[Int() (1)]\n")
expect_emission_failure(integer_overflow emit "inc[9223372036854775807]\n")
expect_emission_failure(integer_underflow emit "add[-9223372036854775808 -1]\n")
expect_emission_failure(late_root_failure emit "iota[2]\ninc[9223372036854775807]\n")
expect_emission_failure(bounded_profile_failure emit-bounded-fail "inc[(9223372036854775807)]\n")
expect_emission_failure(bounded_live_failure emit-bounded-live-fail "inc[inc[(1)]]\n")
expect_emission_failure(validation_iota_allocation_failure emit-validation-fail "iota[2]\n")
expect_emission_failure(validation_literal_allocation_failure emit-validation-fail "(1 2)\n")
expect_emission_failure(validation_lifted_allocation_failure emit-validation-fail-second "inc[(1 2)]\n")

set(bounded_source "${work_directory}/bounded-success.bennu")
set(bounded_oracle "${work_directory}/bounded-success.oracle")
set(bounded_c "${work_directory}/bounded-success.c")
set(bounded_executable
  "${work_directory}/bounded-success${BENNU_EXECUTABLE_SUFFIX}")
file(WRITE "${bounded_source}" "inc[inc[(1)]]\n")
execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" oracle-bounded-exact "${bounded_source}" "${bounded_oracle}"
  RESULT_VARIABLE bounded_oracle_exit)
execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" emit-bounded-exact "${bounded_source}" "${bounded_c}"
  RESULT_VARIABLE bounded_emit_exit)
if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
            "${bounded_c}" "/Fe:${bounded_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE bounded_compile_exit)
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
            "${bounded_c}" -o "${bounded_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE bounded_compile_exit)
endif()
execute_process(
  COMMAND "${bounded_executable}"
  RESULT_VARIABLE bounded_run_exit
  OUTPUT_FILE "${work_directory}/bounded-success.generated"
  ERROR_VARIABLE bounded_run_stderr)
execute_process(
  COMMAND "${bounded_executable}"
  RESULT_VARIABLE bounded_repeat_run_exit
  OUTPUT_FILE "${work_directory}/bounded-success-repeat.generated"
  ERROR_VARIABLE bounded_repeat_run_stderr)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
          "${bounded_oracle}" "${work_directory}/bounded-success.generated"
  RESULT_VARIABLE bounded_compare_exit)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
          "${bounded_oracle}" "${work_directory}/bounded-success-repeat.generated"
  RESULT_VARIABLE bounded_repeat_compare_exit)
if(NOT "${bounded_oracle_exit}" STREQUAL "0" OR
   NOT "${bounded_emit_exit}" STREQUAL "0" OR
   NOT "${bounded_compile_exit}" STREQUAL "0" OR
   NOT "${bounded_run_exit}" STREQUAL "0" OR
   NOT "${bounded_repeat_run_exit}" STREQUAL "0" OR
   NOT bounded_run_stderr STREQUAL "" OR
   NOT bounded_repeat_run_stderr STREQUAL "" OR
   NOT "${bounded_compare_exit}" STREQUAL "0" OR
   NOT "${bounded_repeat_compare_exit}" STREQUAL "0")
  message(FATAL_ERROR "bounded-v1 differential execution failed")
endif()

set(fault_source "${work_directory}/runtime-allocation-failure.bennu")
set(fault_c "${work_directory}/runtime-allocation-failure.c")
set(fault_executable
  "${work_directory}/runtime-allocation-failure${BENNU_EXECUTABLE_SUFFIX}")
file(WRITE "${fault_source}" "iota[2]\niota[2]\n")
execute_process(
  COMMAND "${BENNU_REWRITE_BACKEND_DRIVER}" emit-runtime-fail-second
          "${fault_source}" "${fault_c}"
  RESULT_VARIABLE fault_emit_exit)
if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
            "${fault_c}" "/Fe:${fault_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE fault_compile_exit)
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
            "${fault_c}" -o "${fault_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE fault_compile_exit)
endif()
execute_process(
  COMMAND "${fault_executable}"
  RESULT_VARIABLE fault_run_exit
  OUTPUT_VARIABLE fault_run_stdout
  ERROR_VARIABLE fault_run_stderr)
if(NOT "${fault_emit_exit}" STREQUAL "0" OR
   NOT "${fault_compile_exit}" STREQUAL "0" OR
   "${fault_run_exit}" STREQUAL "0" OR NOT fault_run_stdout STREQUAL "" OR
   NOT fault_run_stderr MATCHES "allocation_unavailable")
  message(FATAL_ERROR
    "runtime allocation failure was not atomic\nexit: ${fault_run_exit}\n"
    "stdout: [${fault_run_stdout}]\nstderr: [${fault_run_stderr}]")
endif()

if(EXISTS "/dev/full")
  execute_process(
    COMMAND "${corpus_executable}"
    RESULT_VARIABLE stdout_failure_exit
    OUTPUT_FILE "/dev/full"
    ERROR_VARIABLE stdout_failure_stderr)
  if("${stdout_failure_exit}" STREQUAL "0")
    message(FATAL_ERROR "generated rewrite executable ignored stdout failure")
  endif()
endif()

file(REMOVE_RECURSE "${work_directory}")
