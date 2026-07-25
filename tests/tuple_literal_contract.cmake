# TEST-ID: TUP-017-STRICT-C-NATIVE
foreach(required BENNU_EXECUTABLE BENNU_PUBLIC_RESOURCE_FIXTURE
                 BENNU_SOURCE_DIR BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/tuple literal contract")
set(source "${BENNU_SOURCE_DIR}/tests/fixtures/tuple-literals.bennu")
set(expected_file "${BENNU_SOURCE_DIR}/tests/fixtures/tuple-literals.out")
set(emitted_c "${work_directory}/tuple-literals.c")
set(emitted_c_second "${work_directory}/tuple-literals-second.c")
set(emitted_executable
    "${work_directory}/tuple-literals-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(native_executable
    "${work_directory}/tuple-literals-native${BENNU_EXECUTABLE_SUFFIX}")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")
file(READ "${expected_file}" expected)

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${source}"
  RESULT_VARIABLE evaluator_exit OUTPUT_VARIABLE evaluator_stdout
  ERROR_VARIABLE evaluator_stderr)
string(REPLACE "\r\n" "\n" evaluator_stdout "${evaluator_stdout}")
if(NOT "${evaluator_exit}" STREQUAL "0" OR
   NOT evaluator_stdout STREQUAL expected OR
   NOT evaluator_stderr STREQUAL "")
  message(FATAL_ERROR
    "tuple evaluator mismatch\nexit: ${evaluator_exit}\n"
    "stdout: [${evaluator_stdout}]\nstderr: [${evaluator_stderr}]")
endif()

foreach(output IN ITEMS "${emitted_c}" "${emitted_c_second}")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source}" -o "${output}"
    RESULT_VARIABLE emit_exit OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr)
  if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
     NOT emit_stderr STREQUAL "")
    message(FATAL_ERROR
      "tuple emit-c failed\nexit: ${emit_exit}\n"
      "stdout: [${emit_stdout}]\nstderr: [${emit_stderr}]")
  endif()
endforeach()
file(READ "${emitted_c}" emitted_source)
file(READ "${emitted_c_second}" emitted_source_second)
if(NOT emitted_source STREQUAL emitted_source_second OR
   NOT emitted_source MATCHES "BENNU_TUPLE" OR
   NOT emitted_source MATCHES "slot_bytes = 16U" OR
   emitted_source MATCHES "[Rr]eference[_ -]?[Cc]ount")
  message(FATAL_ERROR
    "tuple emitted source is nondeterministic or violates storage policy")
endif()

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  string(REGEX REPLACE "/VC/Tools/.*$" "" vs_root
         "${BENNU_C_COMPILER}")
  file(TO_NATIVE_PATH
       "${vs_root}/Common7/Tools/VsDevCmd.bat" vs_dev_command)
  file(TO_NATIVE_PATH "${BENNU_C_COMPILER}" native_c_compiler)
  file(TO_NATIVE_PATH "${emitted_c}" native_emitted_c)
  file(TO_NATIVE_PATH "${emitted_executable}" native_emitted_executable)
  set(strict_compile_script "${work_directory}/strict-compile.cmd")
  file(WRITE "${strict_compile_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_c_compiler}\" /nologo /std:c11 /W4 /WX /TC \"${native_emitted_c}\" /Fe:\"${native_emitted_executable}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${strict_compile_script}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
            -pedantic-errors "${emitted_c}" -o "${emitted_executable}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)
endif()
if(NOT "${compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "strict tuple C11 compilation failed\nstdout: [${compile_stdout}]\n"
    "stderr: [${compile_stderr}]")
endif()

function(strict_compile_tuple input output label)
  if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
    file(TO_NATIVE_PATH "${input}" native_input)
    file(TO_NATIVE_PATH "${output}" native_output)
    set(script "${work_directory}/${label}-strict-compile.cmd")
    file(WRITE "${script}"
         "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
         "@\"${native_c_compiler}\" /nologo /std:c11 /W4 /WX /TC \"${native_input}\" /Fe:\"${native_output}\"\r\n")
    execute_process(
      COMMAND cmd.exe /d /c "${script}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  else()
    execute_process(
      COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
              -pedantic-errors "${input}" -o "${output}"
      WORKING_DIRECTORY "${work_directory}"
      RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  endif()
  if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR
      "${label} strict C11 compilation failed\nstdout: [${stdout}]\n"
      "stderr: [${stderr}]")
  endif()
endfunction()

set(probe_names tuple-limit fault-vector fault-inner fault-outer)
set(probe_arguments)
foreach(probe IN LISTS probe_names)
  set(probe_c "${work_directory}/${probe}-probe.c")
  set(probe_native
      "${work_directory}/${probe}-native${BENNU_EXECUTABLE_SUFFIX}")
  list(APPEND probe_arguments "${probe_c}" "${probe_native}")
endforeach()
execute_process(
  COMMAND "${BENNU_PUBLIC_RESOURCE_FIXTURE}" --tuple-issue50
          "${BENNU_C_COMPILER}" ${probe_arguments}
  RESULT_VARIABLE probe_fixture_exit OUTPUT_VARIABLE probe_fixture_stdout
  ERROR_VARIABLE probe_fixture_stderr)
if(NOT "${probe_fixture_exit}" STREQUAL "0" OR
   NOT probe_fixture_stdout STREQUAL "" OR
   NOT probe_fixture_stderr STREQUAL "")
  message(FATAL_ERROR
    "tuple resource/fault fixture failed\nexit: ${probe_fixture_exit}\n"
    "stdout: [${probe_fixture_stdout}]\n"
    "stderr: [${probe_fixture_stderr}]")
endif()

foreach(probe IN LISTS probe_names)
  set(probe_c "${work_directory}/${probe}-probe.c")
  set(probe_strict
      "${work_directory}/${probe}-strict${BENNU_EXECUTABLE_SUFFIX}")
  set(probe_native
      "${work_directory}/${probe}-native${BENNU_EXECUTABLE_SUFFIX}")
  strict_compile_tuple("${probe_c}" "${probe_strict}" "${probe}")
  set(probe_expected_stderr "")
  foreach(probe_executable IN ITEMS "${probe_strict}" "${probe_native}")
    execute_process(
      COMMAND "${probe_executable}"
      RESULT_VARIABLE probe_exit OUTPUT_VARIABLE probe_stdout
      ERROR_VARIABLE probe_stderr)
    string(REPLACE "\r\n" "\n" probe_stderr "${probe_stderr}")
    if(probe_expected_stderr STREQUAL "")
      set(probe_expected_stderr "${probe_stderr}")
    endif()
    if(NOT "${probe_exit}" STREQUAL "0" OR
       NOT probe_stdout STREQUAL "" OR probe_stderr STREQUAL "" OR
       NOT probe_stderr STREQUAL probe_expected_stderr)
      message(FATAL_ERROR
        "${probe} generated/native failure parity mismatch for "
        "${probe_executable}\nexit: ${probe_exit}\n"
        "stdout: [${probe_stdout}]\nstderr: [${probe_stderr}]")
    endif()
  endforeach()
endforeach()

string(REPEAT "[" 512 deep_open)
string(REPEAT "]" 512 deep_close)
set(deep_text "${deep_open}1${deep_close}\n")
set(deep_source "${work_directory}/deep-tuple.bennu")
set(deep_c "${work_directory}/deep-tuple.c")
set(deep_strict
    "${work_directory}/deep-tuple-strict${BENNU_EXECUTABLE_SUFFIX}")
set(deep_native
    "${work_directory}/deep-tuple-native${BENNU_EXECUTABLE_SUFFIX}")
file(WRITE "${deep_source}" "${deep_text}")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${deep_source}"
  RESULT_VARIABLE deep_eval_exit OUTPUT_VARIABLE deep_eval_stdout
  ERROR_VARIABLE deep_eval_stderr)
string(REPLACE "\r\n" "\n" deep_eval_stdout "${deep_eval_stdout}")
if(NOT "${deep_eval_exit}" STREQUAL "0" OR
   NOT deep_eval_stdout STREQUAL deep_text OR NOT deep_eval_stderr STREQUAL "")
  message(FATAL_ERROR
    "deep tuple evaluator journey failed\nexit: ${deep_eval_exit}\n"
    "stdout: [${deep_eval_stdout}]\nstderr: [${deep_eval_stderr}]")
endif()
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${deep_source}" -o "${deep_c}"
  RESULT_VARIABLE deep_emit_exit OUTPUT_VARIABLE deep_emit_stdout
  ERROR_VARIABLE deep_emit_stderr)
if(NOT "${deep_emit_exit}" STREQUAL "0" OR
   NOT deep_emit_stdout STREQUAL "" OR NOT deep_emit_stderr STREQUAL "")
  message(FATAL_ERROR
    "deep tuple emit failed\nexit: ${deep_emit_exit}\n"
    "stdout: [${deep_emit_stdout}]\nstderr: [${deep_emit_stderr}]")
endif()
strict_compile_tuple("${deep_c}" "${deep_strict}" "deep-tuple")

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  file(TO_NATIVE_PATH "${BENNU_EXECUTABLE}" native_bennu)
  file(TO_NATIVE_PATH "${source}" native_source)
  file(TO_NATIVE_PATH "${native_executable}" native_output)
  set(native_build_script "${work_directory}/native-build.cmd")
  file(WRITE "${native_build_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_bennu}\" build \"${native_source}\" -o \"${native_output}\" --cc \"${native_c_compiler}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${native_build_script}"
    RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
else()
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" build "${source}" -o "${native_executable}"
            --cc "${BENNU_C_COMPILER}"
    RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
endif()
if(NOT "${build_exit}" STREQUAL "0" OR NOT build_stdout STREQUAL "" OR
   NOT build_stderr STREQUAL "")
  message(FATAL_ERROR
    "tuple native build failed\nexit: ${build_exit}\n"
    "stdout: [${build_stdout}]\nstderr: [${build_stderr}]")
endif()

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  file(TO_NATIVE_PATH "${deep_source}" native_deep_source)
  file(TO_NATIVE_PATH "${deep_native}" native_deep_output)
  set(deep_native_build_script "${work_directory}/deep-native-build.cmd")
  file(WRITE "${deep_native_build_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_bennu}\" build \"${native_deep_source}\" -o \"${native_deep_output}\" --cc \"${native_c_compiler}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${deep_native_build_script}"
    RESULT_VARIABLE deep_build_exit OUTPUT_VARIABLE deep_build_stdout
    ERROR_VARIABLE deep_build_stderr)
else()
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" build "${deep_source}" -o "${deep_native}"
            --cc "${BENNU_C_COMPILER}"
    RESULT_VARIABLE deep_build_exit OUTPUT_VARIABLE deep_build_stdout
    ERROR_VARIABLE deep_build_stderr)
endif()
if(NOT "${deep_build_exit}" STREQUAL "0" OR
   NOT deep_build_stdout STREQUAL "" OR NOT deep_build_stderr STREQUAL "")
  message(FATAL_ERROR
    "deep tuple native build failed\nexit: ${deep_build_exit}\n"
    "stdout: [${deep_build_stdout}]\nstderr: [${deep_build_stderr}]")
endif()

foreach(executable IN ITEMS "${emitted_executable}" "${native_executable}")
  execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE run_exit OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr)
  string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
  if(NOT "${run_exit}" STREQUAL "0" OR
     NOT run_stdout STREQUAL expected OR NOT run_stderr STREQUAL "")
    message(FATAL_ERROR
      "tuple native differential mismatch for ${executable}\n"
      "exit: ${run_exit}\nstdout: [${run_stdout}]\n"
      "stderr: [${run_stderr}]")
  endif()
endforeach()

foreach(executable IN ITEMS "${deep_strict}" "${deep_native}")
  execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE deep_run_exit OUTPUT_VARIABLE deep_run_stdout
    ERROR_VARIABLE deep_run_stderr)
  string(REPLACE "\r\n" "\n" deep_run_stdout "${deep_run_stdout}")
  if(NOT "${deep_run_exit}" STREQUAL "0" OR
     NOT deep_run_stdout STREQUAL deep_text OR NOT deep_run_stderr STREQUAL "")
    message(FATAL_ERROR
      "deep tuple generated/native mismatch for ${executable}\n"
      "exit: ${deep_run_exit}\nstdout: [${deep_run_stdout}]\n"
      "stderr: [${deep_run_stderr}]")
  endif()
endforeach()

file(REMOVE_RECURSE "${work_directory}")
