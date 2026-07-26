# TEST-ID: FAN-016-STRICT-C-NATIVE
# TEST-ID: FAN-003-STATIC-ALL-BRANCHES-CLI
# TEST-ID: FAN-005-OPERAND-IDENTITY-C
# TEST-ID: FAN-008-TRANSFER-CLEANUP-NATIVE
# TEST-ID: FAN-011-PROFILE-EVENTS-C
# TEST-ID: FAN-012-ALLOCATION-ORDINALS-C
# TEST-ID: FAN-015-ATOMIC-OUTPUT-NATIVE
foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR BENNU_C_COMPILER
                 BENNU_C_COMPILER_ID BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/fanout contract")
set(source "${BENNU_SOURCE_DIR}/tests/fixtures/fanout.bennu")
set(expected_file "${BENNU_SOURCE_DIR}/tests/fixtures/fanout.out")
set(emitted_c "${work_directory}/fanout.c")
set(emitted_c_second "${work_directory}/fanout-second.c")
set(emitted_executable
    "${work_directory}/fanout-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(native_executable
    "${work_directory}/fanout-native${BENNU_EXECUTABLE_SUFFIX}")
set(failure_source
    "${BENNU_SOURCE_DIR}/tests/fixtures/fanout-prefix-failure.bennu")
set(failure_c "${work_directory}/fanout-prefix-failure.c")
set(failure_emitted_executable
    "${work_directory}/fanout-prefix-failure-emitted${BENNU_EXECUTABLE_SUFFIX}")
set(failure_native_executable
    "${work_directory}/fanout-prefix-failure-native${BENNU_EXECUTABLE_SUFFIX}")
set(failure_probe_c "${work_directory}/fanout-prefix-cleanup-probe.c")
set(failure_probe_executable
    "${work_directory}/fanout-prefix-cleanup-probe${BENNU_EXECUTABLE_SUFFIX}")
set(success_probe_c "${work_directory}/fanout-success-resource-probe.c")
set(success_probe_executable
    "${work_directory}/fanout-success-resource-probe${BENNU_EXECUTABLE_SUFFIX}")
set(fault_source "${work_directory}/fanout-fault-matrix.bennu")
set(fault_c "${work_directory}/fanout-fault-matrix.c")
set(fault_probe_c "${work_directory}/fanout-fault-matrix-probe.c")
set(fault_probe_executable
    "${work_directory}/fanout-fault-matrix-probe${BENNU_EXECUTABLE_SUFFIX}")
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
    "fanout evaluator mismatch\nexit: ${evaluator_exit}\n"
    "stdout: [${evaluator_stdout}]\nstderr: [${evaluator_stderr}]")
endif()

foreach(output IN ITEMS "${emitted_c}" "${emitted_c_second}")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" emit-c "${source}" -o "${output}"
    RESULT_VARIABLE emit_exit OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr)
  if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
     NOT emit_stderr STREQUAL "")
    message(FATAL_ERROR "fanout emit-c failed: ${emit_stderr}")
  endif()
endforeach()
file(READ "${emitted_c}" emitted_source)
file(READ "${emitted_c_second}" emitted_source_second)
string(FIND "${emitted_source}" "\"fanout\"" table_at)
string(FIND "${emitted_source}"
  "if (!bennu_apply(&bennu_resources, BENNU_IMPL_INC_INT" branch_at)
string(FIND "${emitted_source}"
  "  bennu_tuple_transfer(&bennu_values" transfer_at)
if(NOT emitted_source STREQUAL emitted_source_second OR
   table_at LESS 0 OR branch_at LESS table_at OR transfer_at LESS branch_at OR
   emitted_source MATCHES "[Rr]eference[_ -]?[Cc]ount" OR
   emitted_source MATCHES "pthread|std::thread")
  message(FATAL_ERROR
    "fanout emitted source is nondeterministic or violates sequential policy")
endif()
string(FIND "${emitted_source}" "result->cleanup_index = 0U;" zero_prefix_at)
string(FIND "${emitted_source}" "result->cleanup_index += 1U;"
       prefix_increment_at)
if(zero_prefix_at LESS 0 OR prefix_increment_at LESS zero_prefix_at)
  message(FATAL_ERROR
    "fanout emitted source lacks initialized-prefix cleanup state")
endif()
string(REGEX MATCHALL
  "bennu_apply\\(&bennu_resources, BENNU_IMPL_(INC|ADD)_INT, &bennu_values\\[[0-9]+\\], &bennu_values\\[8\\]"
  shared_operand_uses "${emitted_source}")
list(LENGTH shared_operand_uses shared_operand_use_count)
if(NOT shared_operand_use_count EQUAL 2)
  message(FATAL_ERROR
    "fanout backend does not borrow the one iota operand identity in both branches")
endif()
file(WRITE "${success_probe_c}"
     "#include <stddef.h>\n"
     "static void *bennu_probe_malloc(size_t size);\n"
     "static void bennu_probe_free(void *data);\n"
     "#define BENNU_RUNTIME_MALLOC(size) bennu_probe_malloc(size)\n"
     "#define BENNU_RUNTIME_FREE(data) bennu_probe_free(data)\n"
     "#define BENNU_CUSTOM_MAIN\n")
file(APPEND "${success_probe_c}" "${emitted_source}")
file(APPEND "${success_probe_c}"
     "\nstatic size_t bennu_probe_allocations = 0U;\n"
     "static size_t bennu_probe_releases = 0U;\n"
     "static size_t bennu_probe_live = 0U;\n"
     "static void *bennu_probe_malloc(size_t size) {\n"
     "  void *data = malloc(size);\n"
     "  if (data != NULL) {\n"
     "    ++bennu_probe_allocations;\n"
     "    ++bennu_probe_live;\n"
     "  }\n"
     "  return data;\n"
     "}\n"
     "static void bennu_probe_free(void *data) {\n"
     "  if (data != NULL) {\n"
     "    ++bennu_probe_releases;\n"
     "    if (bennu_probe_live != 0U) { --bennu_probe_live; }\n"
     "  }\n"
     "  free(data);\n"
     "}\n"
     "int main(void) {\n"
     "  BennuResources snapshot = {0};\n"
     "  const int status = bennu_execute(&snapshot);\n"
     "  return status == 0 && snapshot.failure == BENNU_FAILURE_NONE &&\n"
     "    snapshot.live_bytes == 0U && snapshot.work_units == 14U &&\n"
     "    snapshot.reservation_ordinal == 6U &&\n"
     "    bennu_probe_allocations == 6U && bennu_probe_releases == 6U &&\n"
     "    bennu_probe_live == 0U ? 0 : 2;\n"
     "}\n")

file(WRITE "${fault_source}"
  "fanout[iota[3] {inc[_]} {add[_ 10]}]\n")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${fault_source}" -o "${fault_c}"
  RESULT_VARIABLE fault_emit_exit OUTPUT_VARIABLE fault_emit_stdout
  ERROR_VARIABLE fault_emit_stderr)
if(NOT "${fault_emit_exit}" STREQUAL "0" OR
   NOT fault_emit_stdout STREQUAL "" OR NOT fault_emit_stderr STREQUAL "")
  message(FATAL_ERROR
    "fanout allocation-fault matrix emit-c failed: ${fault_emit_stderr}")
endif()
file(READ "${fault_c}" fault_emitted_source)
set(fault_initializer_tail
  ";\n  (void)bennu_literal;")
string(CONCAT fault_initializer_instrumented
  ";\n  if (snapshot != NULL && snapshot->has_failure_ordinal) {\n"
  "    bennu_resources.has_failure_ordinal = 1;\n"
  "    bennu_resources.failure_ordinal = snapshot->failure_ordinal;\n"
  "  }\n"
  "  (void)bennu_literal;")
string(REPLACE "${fault_initializer_tail}"
  "${fault_initializer_instrumented}" fault_emitted_source
  "${fault_emitted_source}")
string(FIND "${fault_emitted_source}"
  "bennu_resources.has_failure_ordinal = 1;" fault_instrumentation_at)
if(fault_instrumentation_at LESS 0)
  message(FATAL_ERROR "fanout allocation-fault instrumentation is empty")
endif()
file(WRITE "${fault_probe_c}" "#define BENNU_CUSTOM_MAIN\n")
file(APPEND "${fault_probe_c}" "${fault_emitted_source}")
file(APPEND "${fault_probe_c}"
  "\nint main(void) {\n"
  "  static const size_t expected_work[4] = {0U, 3U, 3U, 6U};\n"
  "  size_t ordinal = 0U;\n"
  "  for (; ordinal < 4U; ++ordinal) {\n"
  "    BennuResources snapshot = {0};\n"
  "    snapshot.has_failure_ordinal = 1;\n"
  "    snapshot.failure_ordinal = ordinal;\n"
  "    if (bennu_execute(&snapshot) != 1 ||\n"
  "        snapshot.failure != BENNU_FAILURE_ALLOCATION ||\n"
  "        snapshot.reservation_ordinal != ordinal + 1U ||\n"
  "        snapshot.work_units != expected_work[ordinal] ||\n"
  "        snapshot.live_bytes != 0U) { return 2; }\n"
  "  }\n"
  "  return 0;\n"
  "}\n")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${failure_source}" -o "${failure_c}"
  RESULT_VARIABLE failure_emit_exit OUTPUT_VARIABLE failure_emit_stdout
  ERROR_VARIABLE failure_emit_stderr)
if(NOT "${failure_emit_exit}" STREQUAL "0" OR
   NOT failure_emit_stdout STREQUAL "" OR
   NOT failure_emit_stderr STREQUAL "")
  message(FATAL_ERROR
    "fanout prefix-failure emit-c failed: ${failure_emit_stderr}")
endif()
file(READ "${failure_c}" failure_emitted_source)
file(WRITE "${failure_probe_c}"
     "#include <stddef.h>\n"
     "static void *bennu_probe_malloc(size_t size);\n"
     "static void bennu_probe_free(void *data);\n"
     "#define BENNU_RUNTIME_MALLOC(size) bennu_probe_malloc(size)\n"
     "#define BENNU_RUNTIME_FREE(data) bennu_probe_free(data)\n"
     "#define BENNU_CUSTOM_MAIN\n")
file(APPEND "${failure_probe_c}" "${failure_emitted_source}")
file(APPEND "${failure_probe_c}"
     "\nstatic size_t bennu_probe_allocations = 0U;\n"
     "static size_t bennu_probe_releases = 0U;\n"
     "static size_t bennu_probe_live = 0U;\n"
     "static void *bennu_probe_malloc(size_t size) {\n"
     "  void *data = malloc(size);\n"
     "  if (data != NULL) {\n"
     "    ++bennu_probe_allocations;\n"
     "    ++bennu_probe_live;\n"
     "  }\n"
     "  return data;\n"
     "}\n"
     "static void bennu_probe_free(void *data) {\n"
     "  if (data != NULL) {\n"
     "    ++bennu_probe_releases;\n"
     "    if (bennu_probe_live != 0U) { --bennu_probe_live; }\n"
     "  }\n"
     "  free(data);\n"
     "}\n"
     "int main(void) {\n"
     "  BennuResources snapshot = {0};\n"
     "  const int status = bennu_execute(&snapshot);\n"
     "  return status == 1 && bennu_probe_allocations == bennu_probe_releases "
     "&& bennu_probe_live == 0U && snapshot.live_bytes == 0U "
     "&& snapshot.work_units == 4U && snapshot.reservation_ordinal == 3U "
     "? 0 : 2;\n"
     "}\n")

if(BENNU_C_COMPILER_ID STREQUAL "MSVC")
  string(REGEX REPLACE "/VC/Tools/.*$" "" vs_root "${BENNU_C_COMPILER}")
  file(TO_NATIVE_PATH
       "${vs_root}/Common7/Tools/VsDevCmd.bat" vs_dev_command)
  file(TO_NATIVE_PATH "${BENNU_C_COMPILER}" native_c_compiler)
  file(TO_NATIVE_PATH "${emitted_c}" native_emitted_c)
  file(TO_NATIVE_PATH "${emitted_executable}" native_emitted_executable)
  file(TO_NATIVE_PATH "${BENNU_EXECUTABLE}" native_bennu)
  file(TO_NATIVE_PATH "${source}" native_source)
  file(TO_NATIVE_PATH "${native_executable}" native_output)
  file(TO_NATIVE_PATH "${failure_source}" native_failure_source)
  file(TO_NATIVE_PATH "${failure_c}" native_failure_c)
  file(TO_NATIVE_PATH "${failure_emitted_executable}"
       native_failure_emitted_executable)
  file(TO_NATIVE_PATH "${failure_native_executable}"
       native_failure_native_executable)
  file(TO_NATIVE_PATH "${failure_probe_c}" native_failure_probe_c)
  file(TO_NATIVE_PATH "${failure_probe_executable}"
       native_failure_probe_executable)
  file(TO_NATIVE_PATH "${success_probe_c}" native_success_probe_c)
  file(TO_NATIVE_PATH "${success_probe_executable}"
       native_success_probe_executable)
  file(TO_NATIVE_PATH "${fault_probe_c}" native_fault_probe_c)
  file(TO_NATIVE_PATH "${fault_probe_executable}"
       native_fault_probe_executable)
  set(compile_script "${work_directory}/strict-compile.cmd")
  file(WRITE "${compile_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_c_compiler}\" /nologo /std:c11 /W4 /WX /TC \"${native_emitted_c}\" /Fe:\"${native_emitted_executable}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${compile_script}"
    RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)
  set(build_script "${work_directory}/native-build.cmd")
  file(WRITE "${build_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_bennu}\" build \"${native_source}\" -o \"${native_output}\" --cc \"${native_c_compiler}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${build_script}"
    RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
  set(failure_compile_script
      "${work_directory}/strict-prefix-failure-compile.cmd")
  file(WRITE "${failure_compile_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_c_compiler}\" /nologo /std:c11 /W4 /WX /TC \"${native_failure_c}\" /Fe:\"${native_failure_emitted_executable}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${failure_compile_script}"
    RESULT_VARIABLE failure_compile_exit
    OUTPUT_VARIABLE failure_compile_stdout
    ERROR_VARIABLE failure_compile_stderr)
  set(failure_probe_compile_script
      "${work_directory}/strict-prefix-cleanup-probe-compile.cmd")
  file(WRITE "${failure_probe_compile_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_c_compiler}\" /nologo /std:c11 /W4 /WX /TC \"${native_failure_probe_c}\" /Fe:\"${native_failure_probe_executable}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${failure_probe_compile_script}"
    RESULT_VARIABLE failure_probe_compile_exit
    OUTPUT_VARIABLE failure_probe_compile_stdout
    ERROR_VARIABLE failure_probe_compile_stderr)
  set(success_probe_compile_script
      "${work_directory}/strict-success-resource-probe-compile.cmd")
  file(WRITE "${success_probe_compile_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_c_compiler}\" /nologo /std:c11 /W4 /WX /TC \"${native_success_probe_c}\" /Fe:\"${native_success_probe_executable}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${success_probe_compile_script}"
    RESULT_VARIABLE success_probe_compile_exit
    OUTPUT_VARIABLE success_probe_compile_stdout
    ERROR_VARIABLE success_probe_compile_stderr)
  set(fault_probe_compile_script
      "${work_directory}/strict-fault-matrix-probe-compile.cmd")
  file(WRITE "${fault_probe_compile_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_c_compiler}\" /nologo /std:c11 /W4 /WX /TC \"${native_fault_probe_c}\" /Fe:\"${native_fault_probe_executable}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${fault_probe_compile_script}"
    RESULT_VARIABLE fault_probe_compile_exit
    OUTPUT_VARIABLE fault_probe_compile_stdout
    ERROR_VARIABLE fault_probe_compile_stderr)
  set(failure_build_script
      "${work_directory}/native-prefix-failure-build.cmd")
  file(WRITE "${failure_build_script}"
       "@call \"${vs_dev_command}\" -arch=x64 -host_arch=x64 >nul\r\n"
       "@\"${native_bennu}\" build \"${native_failure_source}\" -o \"${native_failure_native_executable}\" --cc \"${native_c_compiler}\"\r\n")
  execute_process(
    COMMAND cmd.exe /d /c "${failure_build_script}"
    RESULT_VARIABLE failure_build_exit
    OUTPUT_VARIABLE failure_build_stdout
    ERROR_VARIABLE failure_build_stderr)
else()
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
            -pedantic-errors "${emitted_c}" -o "${emitted_executable}"
    RESULT_VARIABLE compile_exit OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" build "${source}" -o "${native_executable}"
            --cc "${BENNU_C_COMPILER}"
    RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
            -pedantic-errors "${failure_c}" -o
            "${failure_emitted_executable}"
    RESULT_VARIABLE failure_compile_exit
    OUTPUT_VARIABLE failure_compile_stdout
    ERROR_VARIABLE failure_compile_stderr)
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
            -pedantic-errors "${failure_probe_c}" -o
            "${failure_probe_executable}"
    RESULT_VARIABLE failure_probe_compile_exit
    OUTPUT_VARIABLE failure_probe_compile_stdout
    ERROR_VARIABLE failure_probe_compile_stderr)
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
            -pedantic-errors "${success_probe_c}" -o
            "${success_probe_executable}"
    RESULT_VARIABLE success_probe_compile_exit
    OUTPUT_VARIABLE success_probe_compile_stdout
    ERROR_VARIABLE success_probe_compile_stderr)
  execute_process(
    COMMAND "${BENNU_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
            -pedantic-errors "${fault_probe_c}" -o
            "${fault_probe_executable}"
    RESULT_VARIABLE fault_probe_compile_exit
    OUTPUT_VARIABLE fault_probe_compile_stdout
    ERROR_VARIABLE fault_probe_compile_stderr)
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" build "${failure_source}" -o
            "${failure_native_executable}" --cc "${BENNU_C_COMPILER}"
    RESULT_VARIABLE failure_build_exit
    OUTPUT_VARIABLE failure_build_stdout
    ERROR_VARIABLE failure_build_stderr)
endif()
if(NOT "${compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "strict fanout C11 compilation failed\n${compile_stdout}\n${compile_stderr}")
endif()
if(NOT "${build_exit}" STREQUAL "0" OR NOT build_stdout STREQUAL "" OR
   NOT build_stderr STREQUAL "")
  message(FATAL_ERROR
    "fanout native build failed\n${build_stdout}\n${build_stderr}")
endif()
if(NOT "${failure_compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "strict fanout prefix-failure C11 compilation failed\n"
    "${failure_compile_stdout}\n${failure_compile_stderr}")
endif()
if(NOT "${failure_probe_compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "strict fanout prefix-cleanup probe C11 compilation failed\n"
    "${failure_probe_compile_stdout}\n${failure_probe_compile_stderr}")
endif()
if(NOT "${success_probe_compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "strict fanout success resource probe C11 compilation failed\n"
    "${success_probe_compile_stdout}\n${success_probe_compile_stderr}")
endif()
if(NOT "${fault_probe_compile_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "strict fanout allocation-fault matrix C11 compilation failed\n"
    "${fault_probe_compile_stdout}\n${fault_probe_compile_stderr}")
endif()
if(NOT "${failure_build_exit}" STREQUAL "0" OR
   NOT failure_build_stdout STREQUAL "" OR
   NOT failure_build_stderr STREQUAL "")
  message(FATAL_ERROR
    "fanout prefix-failure native build failed\n"
    "${failure_build_stdout}\n${failure_build_stderr}")
endif()

foreach(executable IN ITEMS "${emitted_executable}" "${native_executable}")
  execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE run_exit OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr)
  string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
  if(NOT "${run_exit}" STREQUAL "0" OR NOT run_stdout STREQUAL expected OR
     NOT run_stderr STREQUAL "")
    message(FATAL_ERROR
      "fanout backend mismatch for ${executable}\n"
      "stdout: [${run_stdout}]\nstderr: [${run_stderr}]")
  endif()
endforeach()

execute_process(
  COMMAND "${success_probe_executable}"
  RESULT_VARIABLE success_probe_exit
  OUTPUT_VARIABLE success_probe_stdout
  ERROR_VARIABLE success_probe_stderr)
string(REPLACE "\r\n" "\n" success_probe_stdout "${success_probe_stdout}")
if(NOT "${success_probe_exit}" STREQUAL "0" OR
   NOT success_probe_stdout STREQUAL expected OR
   NOT success_probe_stderr STREQUAL "")
  message(FATAL_ERROR
    "fanout success resource/identity probe failed\n"
    "exit: ${success_probe_exit}\nstdout: [${success_probe_stdout}]\n"
    "stderr: [${success_probe_stderr}]")
endif()

execute_process(
  COMMAND "${fault_probe_executable}"
  RESULT_VARIABLE fault_probe_exit
  OUTPUT_VARIABLE fault_probe_stdout
  ERROR_VARIABLE fault_probe_stderr)
if(NOT "${fault_probe_exit}" STREQUAL "0" OR
   NOT fault_probe_stdout STREQUAL "")
  message(FATAL_ERROR
    "fanout operand/table/branch allocation-fault matrix failed\n"
    "exit: ${fault_probe_exit}\nstdout: [${fault_probe_stdout}]\n"
    "stderr: [${fault_probe_stderr}]")
endif()

set(expected_failure_suffix
    "2:65: DomainError: inc failed: integer_overflow")
execute_process(
  COMMAND "${failure_probe_executable}"
  RESULT_VARIABLE failure_probe_exit
  OUTPUT_VARIABLE failure_probe_stdout
  ERROR_VARIABLE failure_probe_stderr)
string(FIND "${failure_probe_stderr}" "${expected_failure_suffix}"
       failure_probe_error_at)
if(NOT "${failure_probe_exit}" STREQUAL "0" OR
   NOT failure_probe_stdout STREQUAL "" OR
   failure_probe_error_at LESS 0)
  message(FATAL_ERROR
    "fanout prefix-only allocation cleanup probe failed\n"
    "exit: ${failure_probe_exit}\nstdout: [${failure_probe_stdout}]\n"
    "stderr: [${failure_probe_stderr}]")
endif()

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${failure_source}"
  RESULT_VARIABLE failure_evaluator_exit
  OUTPUT_VARIABLE failure_evaluator_stdout
  ERROR_VARIABLE failure_evaluator_stderr)
string(FIND "${failure_evaluator_stderr}" "${expected_failure_suffix}"
       evaluator_failure_at)
if("${failure_evaluator_exit}" STREQUAL "0" OR
   NOT failure_evaluator_stdout STREQUAL "" OR
   evaluator_failure_at LESS 0)
  message(FATAL_ERROR
    "fanout evaluator prefix-failure contract mismatch\n"
    "exit: ${failure_evaluator_exit}\n"
    "stdout: [${failure_evaluator_stdout}]\n"
    "stderr: [${failure_evaluator_stderr}]")
endif()
foreach(executable IN ITEMS
        "${failure_emitted_executable}" "${failure_native_executable}")
  execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE failure_run_exit
    OUTPUT_VARIABLE failure_run_stdout
    ERROR_VARIABLE failure_run_stderr)
  string(FIND "${failure_run_stderr}" "${expected_failure_suffix}"
         backend_failure_at)
  if("${failure_run_exit}" STREQUAL "0" OR
     NOT failure_run_stdout STREQUAL "" OR
     backend_failure_at LESS 0)
    message(FATAL_ERROR
      "fanout prefix-only cleanup backend mismatch for ${executable}\n"
      "exit: ${failure_run_exit}\nstdout: [${failure_run_stdout}]\n"
      "stderr: [${failure_run_stderr}]\n"
      "expected stderr: [${failure_evaluator_stderr}]")
  endif()
endforeach()

# Public static-validation winner and publication atomicity across run,
# emit-c, and build. The earlier branch is dynamically invalid, but the later
# branch's static type error must win before execution or artifact mutation.
set(static_source "${work_directory}/fanout-static-winner.bennu")
set(static_c "${work_directory}/fanout-static-winner.c")
set(static_native
    "${work_directory}/fanout-static-winner${BENNU_EXECUTABLE_SUFFIX}")
file(WRITE "${static_source}"
  "fanout[9223372036854775807 {inc[_]} {add[_ true]}]\n")
set(static_suffix
  ":1:44: TypeError: add arguments do not match an accepted signature; first unsupported argument is 2\n")

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${static_source}"
  RESULT_VARIABLE static_run_exit
  OUTPUT_VARIABLE static_run_stdout
  ERROR_VARIABLE static_run_stderr)
string(REPLACE "\r\n" "\n" static_run_stderr "${static_run_stderr}")
string(FIND "${static_run_stderr}" "${static_suffix}" static_run_error_at)
if("${static_run_exit}" STREQUAL "0" OR
   NOT static_run_stdout STREQUAL "" OR static_run_error_at LESS 0)
  message(FATAL_ERROR
    "fanout run static winner mismatch\nexit: ${static_run_exit}\n"
    "stdout: [${static_run_stdout}]\nstderr: [${static_run_stderr}]")
endif()

file(WRITE "${static_c}" "preserved C sentinel\n")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${static_source}" -o "${static_c}"
  RESULT_VARIABLE static_emit_exit
  OUTPUT_VARIABLE static_emit_stdout
  ERROR_VARIABLE static_emit_stderr)
string(REPLACE "\r\n" "\n" static_emit_stderr "${static_emit_stderr}")
file(READ "${static_c}" static_c_after)
file(GLOB static_c_orphans "${static_c}.tmp*")
string(FIND "${static_emit_stderr}" "${static_suffix}" static_emit_error_at)
if("${static_emit_exit}" STREQUAL "0" OR
   NOT static_emit_stdout STREQUAL "" OR static_emit_error_at LESS 0 OR
   NOT static_c_after STREQUAL "preserved C sentinel\n" OR
   static_c_orphans)
  message(FATAL_ERROR
    "fanout emit-c static winner was not exact and atomic\n"
    "exit: ${static_emit_exit}\nstdout: [${static_emit_stdout}]\n"
    "stderr: [${static_emit_stderr}]\noutput: [${static_c_after}]\n"
    "orphans: [${static_c_orphans}]")
endif()

file(WRITE "${static_native}" "preserved native sentinel\n")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" build "${static_source}" -o
          "${static_native}" --cc "${BENNU_C_COMPILER}"
  RESULT_VARIABLE static_build_exit
  OUTPUT_VARIABLE static_build_stdout
  ERROR_VARIABLE static_build_stderr)
string(REPLACE "\r\n" "\n" static_build_stderr "${static_build_stderr}")
file(READ "${static_native}" static_native_after)
file(GLOB static_native_orphans "${static_native}.tmp*")
string(FIND "${static_build_stderr}" "${static_suffix}"
       static_build_error_at)
if("${static_build_exit}" STREQUAL "0" OR
   NOT static_build_stdout STREQUAL "" OR static_build_error_at LESS 0 OR
   NOT static_native_after STREQUAL "preserved native sentinel\n" OR
   static_native_orphans)
  message(FATAL_ERROR
    "fanout build static winner was not exact and atomic\n"
    "exit: ${static_build_exit}\nstdout: [${static_build_stdout}]\n"
    "stderr: [${static_build_stderr}]\n"
    "output: [${static_native_after}]\n"
    "orphans: [${static_native_orphans}]")
endif()

set(atomic_source "${work_directory}/fanout-later-root-failure.bennu")
file(WRITE "${atomic_source}"
  "fanout[iota[3] {inc[_]}]\n"
  "fanout[9223372036854775807 {inc[_]}]\n")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${atomic_source}"
  RESULT_VARIABLE atomic_exit OUTPUT_VARIABLE atomic_stdout
  ERROR_VARIABLE atomic_stderr)
string(REPLACE "\r\n" "\n" atomic_stderr "${atomic_stderr}")
string(FIND "${atomic_stderr}"
  ":2:29: DomainError: inc failed: integer_overflow"
  atomic_error_at)
if("${atomic_exit}" STREQUAL "0" OR NOT atomic_stdout STREQUAL "" OR
   atomic_error_at LESS 0)
  message(FATAL_ERROR
    "fanout later-root failure published partial stdout\n"
    "exit: ${atomic_exit}\nstdout: [${atomic_stdout}]\n"
    "stderr: [${atomic_stderr}]")
endif()
