foreach(required BENNU_EXECUTABLE BENNU_FAKE_COMPILER BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work "${CMAKE_CURRENT_BINARY_DIR}/source output alias contract")
set(source "${work}/Source Alias.bennu")
set(nested "${work}/nested")
set(trace "${work}/compiler trace.txt")
set(source_bytes "ioata 5\ninc 5\n")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}" "${nested}")

function(assert_no_alias_artifacts context)
  file(GLOB_RECURSE leftovers
    "${work}/*.tmp" "${work}/*.tmp.*"
    "${work}/*.bennu-build.tmp*" "${work}/*.bennu-native.tmp*")
  if(EXISTS "${trace}" OR leftovers)
    message(FATAL_ERROR
      "${context}: alias rejection left compiler/publication artifacts\n"
      "trace exists: ${trace}\nleftovers: [${leftovers}]")
  endif()
endfunction()

function(check_alias_rejection context command source_argument output_argument)
  file(WRITE "${source}" "${source_bytes}")
  file(SHA256 "${source}" hash_before)
  file(REMOVE "${trace}")

  if(command STREQUAL "build")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env
              "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
              "${BENNU_EXECUTABLE}" build "${source_argument}" -o
              "${output_argument}" --cc "${BENNU_FAKE_COMPILER}"
      WORKING_DIRECTORY "${work}"
      RESULT_VARIABLE actual_exit
      OUTPUT_VARIABLE actual_stdout
      ERROR_VARIABLE actual_stderr)
  else()
    execute_process(
      COMMAND "${BENNU_EXECUTABLE}" emit-c "${source_argument}" -o
              "${output_argument}"
      WORKING_DIRECTORY "${work}"
      RESULT_VARIABLE actual_exit
      OUTPUT_VARIABLE actual_stdout
      ERROR_VARIABLE actual_stderr)
  endif()

  string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
  string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")
  if(EXISTS "${source}")
    file(SHA256 "${source}" hash_after)
  else()
    set(hash_after "<missing>")
  endif()
  if("${actual_exit}" STREQUAL "0" OR NOT actual_stdout STREQUAL "" OR
     NOT actual_stderr STREQUAL
       "error: source/output alias: output path refers to input source\n" OR
     NOT hash_after STREQUAL hash_before)
    message(FATAL_ERROR
      "${context}: source/output alias contract mismatch\n"
      "exit: ${actual_exit}\nstdout: [${actual_stdout}]\n"
      "stderr: [${actual_stderr}]\n"
      "source hash before: ${hash_before}\nafter: ${hash_after}")
  endif()
  assert_no_alias_artifacts("${context}")
endfunction()

check_alias_rejection("emit-c exact spelling" emit-c "${source}" "${source}")
check_alias_rejection("build exact spelling" build "${source}" "${source}")

set(relative_source "Source Alias.bennu")
set(normalized_output "${nested}/../Source Alias.bennu")
check_alias_rejection("emit-c normalized spelling" emit-c
  "${relative_source}" "${normalized_output}")
check_alias_rejection("build normalized spelling" build
  "${relative_source}" "${normalized_output}")

set(hard_link "${work}/hard link alias.bennu")
file(CREATE_LINK "${source}" "${hard_link}" RESULT hard_link_result)
if(NOT hard_link_result STREQUAL "0")
  message(FATAL_ERROR "unable to create hard-link alias: ${hard_link_result}")
endif()
check_alias_rejection("emit-c hard-link identity" emit-c
  "${source}" "${hard_link}")
check_alias_rejection("build hard-link identity" build
  "${source}" "${hard_link}")

if(WIN32)
  string(TOLOWER "${source}" case_variant_output)
  check_alias_rejection("emit-c Windows case spelling" emit-c
    "${source}" "${case_variant_output}")
  check_alias_rejection("build Windows case spelling" build
    "${source}" "${case_variant_output}")
endif()

# The guard must not disturb publish-last replacement for distinct existing files.
file(WRITE "${source}" "${source_bytes}")
file(SHA256 "${source}" distinct_hash_before)
set(distinct_c "${work}/distinct existing.c")
file(WRITE "${distinct_c}" "sentinel C bytes\n")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" emit-c "${source}" -o "${distinct_c}"
  RESULT_VARIABLE distinct_emit_exit OUTPUT_VARIABLE distinct_emit_stdout
  ERROR_VARIABLE distinct_emit_stderr)
file(READ "${distinct_c}" distinct_c_bytes)
file(SHA256 "${source}" distinct_hash_after_emit)
if(NOT "${distinct_emit_exit}" STREQUAL "0" OR
   NOT distinct_emit_stdout STREQUAL "" OR NOT distinct_emit_stderr STREQUAL "" OR
   distinct_c_bytes STREQUAL "sentinel C bytes\n" OR
   NOT distinct_hash_after_emit STREQUAL distinct_hash_before)
  message(FATAL_ERROR "distinct existing emit-c output was not safely replaced")
endif()
assert_no_alias_artifacts("distinct existing emit-c output")

set(distinct_native
    "${work}/distinct existing native${BENNU_EXECUTABLE_SUFFIX}")
file(WRITE "${distinct_native}" "sentinel native bytes\n")
file(REMOVE "${trace}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${source}" -o "${distinct_native}"
          --cc "${BENNU_FAKE_COMPILER}"
  RESULT_VARIABLE distinct_build_exit OUTPUT_VARIABLE distinct_build_stdout
  ERROR_VARIABLE distinct_build_stderr)
file(READ "${distinct_native}" distinct_native_bytes)
file(SHA256 "${source}" distinct_hash_after_build)
if(NOT "${distinct_build_exit}" STREQUAL "0" OR
   NOT distinct_build_stdout STREQUAL "" OR
   NOT distinct_build_stderr STREQUAL "" OR
   NOT distinct_native_bytes STREQUAL "fake native output\n" OR
   NOT distinct_hash_after_build STREQUAL distinct_hash_before OR
   NOT EXISTS "${trace}")
  message(FATAL_ERROR "distinct existing native output was not safely replaced")
endif()
file(REMOVE "${trace}")
assert_no_alias_artifacts("distinct existing native output")

file(REMOVE_RECURSE "${work}")
