# TEST-ID: PUBLIC-RESOURCE-CROSS-BACKEND
# TEST-ID: PUBLIC-RESOURCE-ALLOCATION
# TEST-ID: PUBLIC-DIRECT-SUCCESS-CORPUS
# TEST-ID: PUBLIC-DIRECT-ERROR-MATRIX
# TEST-ID: PARG-012-RESOURCE-DIAGNOSTICS
foreach(required BENNU_PUBLIC_RESOURCE_FIXTURE BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX BENNU_SOURCE_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/public resource contract")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")

set(profile_c "${work_directory}/profile.c")
set(profile_emitted
  "${work_directory}/profile-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(profile_native
  "${work_directory}/profile-native${BENNU_EXECUTABLE_SUFFIX}")
set(refusal_c "${work_directory}/profile-refusal.c")
set(refusal_emitted
  "${work_directory}/profile-refusal-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(refusal_native
  "${work_directory}/profile-refusal-native${BENNU_EXECUTABLE_SUFFIX}")
set(refusal_expected "${work_directory}/profile-refusal.expected")
set(iota_c "${work_directory}/allocation-iota.c")
set(iota_emitted
  "${work_directory}/allocation-iota-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(iota_native
  "${work_directory}/allocation-iota-native${BENNU_EXECUTABLE_SUFFIX}")
set(lifted_c "${work_directory}/allocation-lifted.c")
set(lifted_emitted
  "${work_directory}/allocation-lifted-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(lifted_native
  "${work_directory}/allocation-lifted-native${BENNU_EXECUTABLE_SUFFIX}")
set(late_c "${work_directory}/allocation-late.c")
set(late_emitted
  "${work_directory}/allocation-late-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(late_native
  "${work_directory}/allocation-late-native${BENNU_EXECUTABLE_SUFFIX}")

execute_process(
  COMMAND "${BENNU_PUBLIC_RESOURCE_FIXTURE}" "${BENNU_C_COMPILER}"
          "${BENNU_SOURCE_DIR}/tests/fixtures/public-path-success.bennu"
          "${BENNU_SOURCE_DIR}/tests/fixtures/public-path-success.out"
          "${profile_c}" "${profile_native}"
          "${iota_c}" "${iota_native}"
          "${lifted_c}" "${lifted_native}"
          "${late_c}" "${late_native}"
          "${refusal_c}" "${refusal_native}" "${refusal_expected}"
  RESULT_VARIABLE fixture_exit OUTPUT_VARIABLE fixture_stdout
  ERROR_VARIABLE fixture_stderr)
if(NOT "${fixture_exit}" STREQUAL "0" OR NOT fixture_stdout STREQUAL "" OR
   NOT fixture_stderr STREQUAL "")
  message(FATAL_ERROR
    "PUBLIC-RESOURCE-MATRIX public API fixture failed\n"
    "exit: ${fixture_exit}\nstdout: [${fixture_stdout}]\n"
    "stderr: [${fixture_stderr}]")
endif()

foreach(c_file profile refusal iota lifted late)
  set(source "${${c_file}_c}")
  set(executable "${${c_file}_emitted}")
  if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" /nologo /std:c11 /W4 /WX
              "${source}" "/Fe:${executable}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr)
  else()
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Wpedantic -Werror
              "${source}" -o "${executable}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
      ERROR_VARIABLE compile_stderr)
  endif()
  if(NOT "${compile_exit}" STREQUAL "0")
    message(FATAL_ERROR
      "PUBLIC-RESOURCE-MATRIX strict compilation failed for ${c_file}\n"
      "stdout: [${compile_stdout}]\nstderr: [${compile_stderr}]")
  endif()
endforeach()

file(READ "${profile_c}" profile_source)
file(READ "${refusal_c}" refusal_source)
file(READ "${iota_c}" iota_source)
file(READ "${lifted_c}" lifted_source)
file(READ "${late_c}" late_source)
if(NOT profile_source MATCHES "1, 1, 1, 8U, 24U, 2U" OR
   NOT refusal_source MATCHES "1, 1, 1, 8U, 24U, 1U" OR
   NOT iota_source MATCHES "0, 0, 0, 0U, 0U, 0U, 0U, 0U, 0U, 1, 0U" OR
   NOT lifted_source MATCHES "0, 0, 0, 0U, 0U, 0U, 0U, 0U, 0U, 1, 1U" OR
   NOT late_source MATCHES "0, 0, 0, 0U, 0U, 0U, 0U, 0U, 0U, 1, 1U")
  message(FATAL_ERROR
    "PUBLIC-RESOURCE-MATRIX generated configuration bytes are incomplete")
endif()
string(FIND "${refusal_source}" "return fflush(stdout) == 0 ? 0 : 1;"
       refusal_success_epilogue)
if(refusal_success_epilogue EQUAL -1)
  message(FATAL_ERROR
    "PUBLIC-RESOURCE-MATRIX refusal artifact was constant-folded during lowering")
endif()

function(check_success name executable expected_stdout invocation)
  execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE run_exit OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr)
  string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
  string(REPLACE "\r\n" "\n" run_stderr "${run_stderr}")
  if(NOT "${run_exit}" STREQUAL "0" OR
     NOT run_stdout STREQUAL expected_stdout OR NOT run_stderr STREQUAL "")
    message(FATAL_ERROR
      "PUBLIC-RESOURCE-MATRIX ${name} invocation ${invocation} mismatch\n"
      "exit: ${run_exit}\nstdout: [${run_stdout}]\nstderr: [${run_stderr}]")
  endif()
endfunction()

# TEST-ID: PUBLIC-RESOURCE-BOUNDED-REFUSAL
function(check_profile_refusal name executable invocation)
  file(READ "${refusal_expected}" expected_stderr)
  execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE run_exit OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr)
  string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
  string(REPLACE "\r\n" "\n" run_stderr "${run_stderr}")
  if("${run_exit}" STREQUAL "0" OR NOT run_stdout STREQUAL "" OR
     NOT run_stderr STREQUAL expected_stderr)
    message(FATAL_ERROR
      "PUBLIC-RESOURCE-MATRIX ${name} invocation ${invocation} refusal mismatch\n"
      "exit: ${run_exit}\nstdout: [${run_stdout}]\n"
      "stderr: [${run_stderr}]\nexpected: [${expected_stderr}]")
  endif()
endfunction()

function(check_allocation_failure name executable expected_stderr)
  foreach(invocation RANGE 1 2)
    execute_process(
      COMMAND "${executable}"
      RESULT_VARIABLE run_exit OUTPUT_VARIABLE run_stdout
      ERROR_VARIABLE run_stderr)
    string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
    string(REPLACE "\r\n" "\n" run_stderr "${run_stderr}")
    if("${run_exit}" STREQUAL "0" OR NOT run_stdout STREQUAL "" OR
       NOT run_stderr STREQUAL expected_stderr)
      message(FATAL_ERROR
        "PUBLIC-RESOURCE-MATRIX ${name} invocation ${invocation} was not atomic\n"
        "exit: ${run_exit}\nstdout: [${run_stdout}]\nstderr: [${run_stderr}]")
    endif()
  endforeach()
endfunction()

# TEST-ID: PUBLIC-RESOURCE-RESET
foreach(bounded_reset_iteration RANGE 1 2)
  check_success(profile-emitted "${profile_emitted}" "(2)\n(2)\n"
                "${bounded_reset_iteration}")
  check_success(profile-native "${profile_native}" "(2)\n(2)\n"
                "${bounded_reset_iteration}")
  check_profile_refusal(refusal-emitted "${refusal_emitted}"
                        "${bounded_reset_iteration}")
  check_profile_refusal(refusal-native "${refusal_native}"
                        "${bounded_reset_iteration}")
endforeach()
foreach(path iota lifted late)
  if(path STREQUAL "iota")
    set(allocation_expected
      "bennu-source:1:1: ResourceError: iota resource request failed: allocation_unavailable\n")
  elseif(path STREQUAL "lifted")
    set(allocation_expected
      "bennu-source:1:1: ResourceError: inc resource request failed: allocation_unavailable\n")
  else()
    set(allocation_expected
      "bennu-source:2:1: ResourceError: iota resource request failed: allocation_unavailable\n")
  endif()
  check_allocation_failure("${path}-emitted" "${${path}_emitted}"
                           "${allocation_expected}")
  check_allocation_failure("${path}-native" "${${path}_native}"
                           "${allocation_expected}")
endforeach()

file(REMOVE_RECURSE "${work_directory}")
