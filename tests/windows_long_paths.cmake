foreach(required BENNU_EXECUTABLE BENNU_FAKE_COMPILER BENNU_EXECUTABLE_SUFFIX
                 BENNU_SOURCE_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

if(NOT WIN32)
  message(FATAL_ERROR "Windows long-path coverage must run on Windows")
endif()

set(work "${CMAKE_CURRENT_BINARY_DIR}/windows long path extracted release")
set(stage "${work}/stage")
set(extracted "${work}/extracted")
set(archive "${work}/bennu-windows-x64.zip")
set(compiler_temp_root "${work}/compiler temp")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${stage}" "${extracted}" "${compiler_temp_root}")

get_filename_component(executable_name "${BENNU_EXECUTABLE}" NAME)
configure_file("${BENNU_EXECUTABLE}" "${stage}/${executable_name}" COPYONLY)
configure_file("${BENNU_SOURCE_DIR}/LICENSE" "${stage}/LICENSE" COPYONLY)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" --format=zip
          "${executable_name}" LICENSE
  WORKING_DIRECTORY "${stage}"
  RESULT_VARIABLE archive_exit OUTPUT_VARIABLE archive_stdout
  ERROR_VARIABLE archive_stderr)
if(NOT "${archive_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "unable to create release-like Windows archive\nstdout: [${archive_stdout}]\n"
    "stderr: [${archive_stderr}]")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar xf "${archive}"
  WORKING_DIRECTORY "${extracted}"
  RESULT_VARIABLE extract_exit OUTPUT_VARIABLE extract_stdout
  ERROR_VARIABLE extract_stderr)
if(NOT "${extract_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "unable to extract release-like Windows archive\nstdout: [${extract_stdout}]\n"
    "stderr: [${extract_stderr}]")
endif()
set(bennu "${extracted}/${executable_name}")
if(NOT EXISTS "${bennu}")
  message(FATAL_ERROR "extracted release executable is missing")
endif()

set(long_parent "${work}/ordinary space éß 文 🐍")
foreach(index RANGE 1 8)
  string(APPEND long_parent
    "/segment ${index} abcdefghijklmnopqrstuvwxyz é 文 🐍")
endforeach()
file(MAKE_DIRECTORY "${long_parent}" "${long_parent}/normalized child")

set(source "${long_parent}/source space éß 文 🐍.bennu")
set(invalid_source "${long_parent}/invalid source éß 文 🐍.bennu")
set(missing_source "${long_parent}/missing source éß 文 🐍.bennu")
set(emitted "${long_parent}/emitted output éß 文 🐍.c")
set(native_output
  "${long_parent}/native output éß 文 🐍${BENNU_EXECUTABLE_SUFFIX}")
set(real_native_output
  "${long_parent}/real native output éß 文 🐍${BENNU_EXECUTABLE_SUFFIX}")
set(short_native_copy
  "${work}/real native verification${BENNU_EXECUTABLE_SUFFIX}")
set(cc_output
  "${long_parent}/CC output éß 文 🐍${BENNU_EXECUTABLE_SUFFIX}")
set(compiler "${long_parent}/selected compiler éß 文 🐍/cl.exe")
set(trace "${long_parent}/compiler trace éß 文 🐍.txt")
set(output_directory "${long_parent}/existing output directory éß 文 🐍")

get_filename_component(compiler_parent "${compiler}" DIRECTORY)
file(MAKE_DIRECTORY "${compiler_parent}" "${output_directory}")
configure_file("${BENNU_FAKE_COMPILER}" "${compiler}" COPYONLY)
file(WRITE "${source}" "iota[5]\nadd[1 2.5]\n")
file(WRITE "${invalid_source}" "inc 5\nwat[1]\n")

foreach(path IN ITEMS "${source}" "${emitted}" "${native_output}"
                      "${real_native_output}" "${compiler}")
  string(LENGTH "${path}" path_length)
  if(path_length LESS_EQUAL 260)
    message(FATAL_ERROR "test path is not longer than 260 characters: ${path}")
  endif()
  string(FIND "${path}" "\\\\?\\" extended_prefix_at)
  if(NOT extended_prefix_at EQUAL -1)
    message(FATAL_ERROR "test path unexpectedly contains caller-supplied prefix: ${path}")
  endif()
endforeach()

function(normalize_newlines variable)
  string(REPLACE "\r\n" "\n" normalized "${${variable}}")
  set(${variable} "${normalized}" PARENT_SCOPE)
endfunction()

function(assert_no_temporary_artifacts context)
  file(GLOB_RECURSE leftovers
    "${long_parent}/*.tmp" "${long_parent}/*.tmp.*"
    "${long_parent}/*.bennu-build.tmp*"
    "${long_parent}/*.bennu-native.tmp*")
  file(GLOB compiler_workspaces "${compiler_temp_root}/bennu-build-*")
  list(APPEND leftovers ${compiler_workspaces})
  if(leftovers)
    message(FATAL_ERROR "${context}: temporary artifacts remain: ${leftovers}")
  endif()
endfunction()

execute_process(
  COMMAND "${bennu}" run "${source}"
  RESULT_VARIABLE run_exit OUTPUT_VARIABLE run_stdout ERROR_VARIABLE run_stderr)
normalize_newlines(run_stdout)
if(NOT "${run_exit}" STREQUAL "0" OR
   NOT run_stdout STREQUAL "(1 2 3 4 5)\n3.5\n" OR
   NOT run_stderr STREQUAL "")
  message(FATAL_ERROR
    "extracted ordinary long-path run failed\nexit: ${run_exit}\n"
    "stdout: [${run_stdout}]\nstderr: [${run_stderr}]")
endif()

execute_process(
  COMMAND "${bennu}" run "${missing_source}"
  RESULT_VARIABLE missing_exit OUTPUT_VARIABLE missing_stdout
  ERROR_VARIABLE missing_stderr)
normalize_newlines(missing_stderr)
if("${missing_exit}" STREQUAL "0" OR NOT missing_stdout STREQUAL "" OR
   NOT missing_stderr STREQUAL
     "${missing_source}:1:1: file error: unable to read source\n")
  message(FATAL_ERROR
    "long missing-source diagnostic mismatch\nexit: ${missing_exit}\n"
    "stdout: [${missing_stdout}]\nstderr: [${missing_stderr}]")
endif()

execute_process(
  COMMAND "${bennu}" emit-c "${source}" -o "${emitted}"
  RESULT_VARIABLE emit_exit OUTPUT_VARIABLE emit_stdout ERROR_VARIABLE emit_stderr)
if(NOT "${emit_exit}" STREQUAL "0" OR NOT emit_stdout STREQUAL "" OR
   NOT emit_stderr STREQUAL "" OR NOT EXISTS "${emitted}")
  message(FATAL_ERROR
    "extracted ordinary long-path emit-c failed\nexit: ${emit_exit}\n"
    "stdout: [${emit_stdout}]\nstderr: [${emit_stderr}]")
endif()
file(READ "${emitted}" emitted_source)
if(NOT emitted_source MATCHES "#include <inttypes.h>" OR
   NOT emitted_source MATCHES "bennu_print_value")
  message(FATAL_ERROR "long emit-c produced unexpected C")
endif()
assert_no_temporary_artifacts("long emit-c success")

file(WRITE "${emitted}" "sentinel C bytes\n")
execute_process(
  COMMAND "${bennu}" emit-c "${invalid_source}" -o "${emitted}"
  RESULT_VARIABLE invalid_exit OUTPUT_VARIABLE invalid_stdout
  ERROR_VARIABLE invalid_stderr)
normalize_newlines(invalid_stderr)
file(READ "${emitted}" preserved_emitted)
if("${invalid_exit}" STREQUAL "0" OR NOT invalid_stdout STREQUAL "" OR
   NOT invalid_stderr STREQUAL
     "${invalid_source}:2:1: UnknownPrimitive: unknown primitive 'wat'\n" OR
   NOT preserved_emitted STREQUAL "sentinel C bytes\n")
  message(FATAL_ERROR
    "long invalid-source publication was not atomic\nexit: ${invalid_exit}\n"
    "stdout: [${invalid_stdout}]\nstderr: [${invalid_stderr}]\n"
    "output: [${preserved_emitted}]")
endif()
assert_no_temporary_artifacts("long invalid-source failure")

execute_process(
  COMMAND "${bennu}" emit-c "${source}" -o "${output_directory}"
  RESULT_VARIABLE emit_publish_exit OUTPUT_VARIABLE emit_publish_stdout
  ERROR_VARIABLE emit_publish_stderr)
normalize_newlines(emit_publish_stderr)
if("${emit_publish_exit}" STREQUAL "0" OR
   NOT emit_publish_stdout STREQUAL "" OR
   NOT emit_publish_stderr STREQUAL
     "${output_directory}:1:1: file error: unable to write output\n" OR
   NOT IS_DIRECTORY "${output_directory}")
  message(FATAL_ERROR
    "long emit-c publication failure mismatch\nexit: ${emit_publish_exit}\n"
    "stdout: [${emit_publish_stdout}]\nstderr: [${emit_publish_stderr}]")
endif()
assert_no_temporary_artifacts("long emit-c publication failure")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=CC
          "TMP=${compiler_temp_root}" "TEMP=${compiler_temp_root}"
          "${bennu}" build "${source}" -o "${real_native_output}"
  RESULT_VARIABLE real_build_exit OUTPUT_VARIABLE real_build_stdout
  ERROR_VARIABLE real_build_stderr)
if(NOT "${real_build_exit}" STREQUAL "0" OR
   NOT real_build_stdout STREQUAL "" OR NOT real_build_stderr STREQUAL "" OR
   NOT EXISTS "${real_native_output}")
  message(FATAL_ERROR
    "extracted real-compiler long-path build failed\nexit: ${real_build_exit}\n"
    "stdout: [${real_build_stdout}]\nstderr: [${real_build_stderr}]")
endif()
file(COPY_FILE "${real_native_output}" "${short_native_copy}" ONLY_IF_DIFFERENT)
execute_process(
  COMMAND "${short_native_copy}"
  RESULT_VARIABLE real_run_exit OUTPUT_VARIABLE real_run_stdout
  ERROR_VARIABLE real_run_stderr)
normalize_newlines(real_run_stdout)
if(NOT "${real_run_exit}" STREQUAL "0" OR
   NOT real_run_stdout STREQUAL "(1 2 3 4 5)\n3.5\n" OR
   NOT real_run_stderr STREQUAL "")
  message(FATAL_ERROR
    "long-path native output failed\nexit: ${real_run_exit}\n"
    "stdout: [${real_run_stdout}]\nstderr: [${real_run_stderr}]")
endif()
assert_no_temporary_artifacts("long real-compiler build success")

file(REMOVE "${trace}" "${native_output}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "TMP=${compiler_temp_root}" "TEMP=${compiler_temp_root}"
          "${bennu}" build "${source}" -o "${native_output}"
          --cc "${compiler}"
  RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout ERROR_VARIABLE build_stderr)
if(NOT "${build_exit}" STREQUAL "0" OR NOT build_stdout STREQUAL "" OR
   NOT build_stderr STREQUAL "" OR NOT EXISTS "${native_output}" OR
   NOT EXISTS "${trace}")
  message(FATAL_ERROR
    "extracted explicit long-compiler build failed\nexit: ${build_exit}\n"
    "stdout: [${build_stdout}]\nstderr: [${build_stderr}]")
endif()
file(READ "${native_output}" native_bytes)
file(READ "${trace}" explicit_trace)
if(NOT native_bytes STREQUAL "fake native output\n" OR
   NOT explicit_trace MATCHES "ordinary space éß 文 🐍" OR
   NOT explicit_trace MATCHES "/Fe:.*program.exe")
  message(FATAL_ERROR
    "long explicit compiler arguments/output mismatch: ${explicit_trace}")
endif()
assert_no_temporary_artifacts("long explicit compiler success")

file(REMOVE "${trace}" "${cc_output}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "CC=${compiler}" "BENNU_FAKE_CC_MODE=success"
          "BENNU_FAKE_CC_TRACE=${trace}"
          "TMP=${compiler_temp_root}" "TEMP=${compiler_temp_root}"
          "${bennu}" build "${source}" -o "${cc_output}"
  RESULT_VARIABLE cc_exit OUTPUT_VARIABLE cc_stdout ERROR_VARIABLE cc_stderr)
if(NOT "${cc_exit}" STREQUAL "0" OR NOT cc_stdout STREQUAL "" OR
   NOT cc_stderr STREQUAL "" OR NOT EXISTS "${cc_output}" OR
   NOT EXISTS "${trace}")
  message(FATAL_ERROR
    "extracted long CC compiler build failed\nexit: ${cc_exit}\n"
    "stdout: [${cc_stdout}]\nstderr: [${cc_stderr}]")
endif()
assert_no_temporary_artifacts("long CC compiler success")

file(WRITE "${native_output}" "sentinel native bytes\n")
file(REMOVE "${trace}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "BENNU_FAKE_CC_MODE=fail" "BENNU_FAKE_CC_TRACE=${trace}"
          "TMP=${compiler_temp_root}" "TEMP=${compiler_temp_root}"
          "${bennu}" build "${source}" -o "${native_output}"
          --cc "${compiler}"
  RESULT_VARIABLE compiler_exit OUTPUT_VARIABLE compiler_stdout
  ERROR_VARIABLE compiler_stderr)
normalize_newlines(compiler_stderr)
file(READ "${native_output}" preserved_native)
if("${compiler_exit}" STREQUAL "0" OR NOT compiler_stdout STREQUAL "" OR
   NOT compiler_stderr MATCHES
     "selected from --cc.*selected compiler éß 文 🐍.*exited with status 23" OR
   NOT preserved_native STREQUAL "sentinel native bytes\n")
  message(FATAL_ERROR
    "long compiler failure was not atomic\nexit: ${compiler_exit}\n"
    "stdout: [${compiler_stdout}]\nstderr: [${compiler_stderr}]\n"
    "output: [${preserved_native}]")
endif()
assert_no_temporary_artifacts("long compiler failure")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BENNU_FAKE_CC_MODE=success"
          "TMP=${compiler_temp_root}" "TEMP=${compiler_temp_root}"
          "${bennu}" build "${source}" -o "${output_directory}"
          --cc "${compiler}"
  RESULT_VARIABLE build_publish_exit OUTPUT_VARIABLE build_publish_stdout
  ERROR_VARIABLE build_publish_stderr)
normalize_newlines(build_publish_stderr)
string(FIND "${build_publish_stderr}" "${output_directory}" output_path_at)
if("${build_publish_exit}" STREQUAL "0" OR
   NOT build_publish_stdout STREQUAL "" OR output_path_at EQUAL -1 OR
   NOT build_publish_stderr MATCHES "unable to replace native output" OR
   NOT IS_DIRECTORY "${output_directory}")
  message(FATAL_ERROR
    "long native publication failure mismatch\nexit: ${build_publish_exit}\n"
    "stdout: [${build_publish_stdout}]\nstderr: [${build_publish_stderr}]")
endif()
assert_no_temporary_artifacts("long native publication failure")

function(check_alias_rejection context command output_argument)
  file(WRITE "${source}" "iota[5]\nadd[1 2.5]\n")
  file(SHA256 "${source}" hash_before)
  file(REMOVE "${trace}")
  if(command STREQUAL "build")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env
              "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
              "TMP=${compiler_temp_root}" "TEMP=${compiler_temp_root}"
              "${bennu}" build "${source}" -o "${output_argument}"
              --cc "${compiler}"
      RESULT_VARIABLE alias_exit OUTPUT_VARIABLE alias_stdout
      ERROR_VARIABLE alias_stderr)
  else()
    execute_process(
      COMMAND "${bennu}" emit-c "${source}" -o "${output_argument}"
      RESULT_VARIABLE alias_exit OUTPUT_VARIABLE alias_stdout
      ERROR_VARIABLE alias_stderr)
  endif()
  normalize_newlines(alias_stderr)
  file(SHA256 "${source}" hash_after)
  if("${alias_exit}" STREQUAL "0" OR NOT alias_stdout STREQUAL "" OR
     NOT alias_stderr STREQUAL
       "error: source/output alias: output path refers to input source\n" OR
     NOT hash_after STREQUAL hash_before OR EXISTS "${trace}")
    message(FATAL_ERROR
      "${context}: long alias rejection mismatch\nexit: ${alias_exit}\n"
      "stdout: [${alias_stdout}]\nstderr: [${alias_stderr}]")
  endif()
  assert_no_temporary_artifacts("${context}")
endfunction()

foreach(command IN ITEMS emit-c build)
  check_alias_rejection("${command} exact long alias" "${command}" "${source}")
  check_alias_rejection("${command} normalized long alias" "${command}"
    "${long_parent}/normalized child/../source space éß 文 🐍.bennu")
endforeach()

set(hard_link "${long_parent}/hard link alias éß 文 🐍.bennu")
file(TO_NATIVE_PATH "${source}" source_native)
file(TO_NATIVE_PATH "${hard_link}" hard_link_native)
set(source_extended "\\\\?\\${source_native}")
set(hard_link_extended "\\\\?\\${hard_link_native}")
file(CREATE_LINK "${source_extended}" "${hard_link_extended}"
  RESULT hard_link_result)
if(NOT hard_link_result STREQUAL "0")
  message(FATAL_ERROR "unable to create long hard-link alias: ${hard_link_result}")
endif()
foreach(command IN ITEMS emit-c build)
  check_alias_rejection("${command} hard-link long alias" "${command}" "${hard_link}")
endforeach()

string(TOLOWER "${source}" case_variant)
foreach(command IN ITEMS emit-c build)
  check_alias_rejection("${command} case-variant long alias" "${command}"
    "${case_variant}")
endforeach()

file(REMOVE_RECURSE "${work}")
