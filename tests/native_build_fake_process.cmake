foreach(required BENNU_EXECUTABLE BENNU_FAKE_COMPILER BENNU_EXECUTABLE_SUFFIX
                 BENNU_FALLBACK_COMPILER_NAME)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work "${CMAKE_CURRENT_BINARY_DIR}/native build fake process")
set(valid_source "${work}/source with spaces & $(touch sentinel).bennu")
set(invalid_source "${work}/invalid source.bennu")
set(output "${work}/native with spaces & $(touch sentinel)${BENNU_EXECUTABLE_SUFFIX}")
set(trace "${work}/compiler trace.txt")
set(sentinel "${work}/sentinel")
if(WIN32)
  set(fake_copy_directory "${work}/fake compiler & $(touch sentinel)")
  set(fake_copy "${fake_copy_directory}/cl.exe")
else()
  set(fake_copy "${work}/fake compiler & $(touch sentinel)")
endif()
set(fake_path_directory "${work}/fallback path")
set(fallback_fake
    "${fake_path_directory}/${BENNU_FALLBACK_COMPILER_NAME}")
set(relative_path_directory "${work}/relative path & $(touch sentinel)")
set(relative_named_fake "${relative_path_directory}/relative-cc")
set(relative_fallback_fake
    "${relative_path_directory}/${BENNU_FALLBACK_COMPILER_NAME}")
set(empty_path "${work}/empty path")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}" "${fake_path_directory}"
                    "${relative_path_directory}" "${empty_path}")
if(WIN32)
  file(MAKE_DIRECTORY "${fake_copy_directory}")
endif()
file(WRITE "${valid_source}" "ioata 5\ninc 5\n")
file(WRITE "${invalid_source}" "inc 5\nwat\n")
configure_file("${BENNU_FAKE_COMPILER}" "${fake_copy}" COPYONLY)
configure_file("${BENNU_FAKE_COMPILER}" "${fallback_fake}" COPYONLY)
configure_file("${BENNU_FAKE_COMPILER}" "${relative_named_fake}" COPYONLY)
configure_file("${BENNU_FAKE_COMPILER}" "${relative_fallback_fake}" COPYONLY)

function(assert_clean context)
  file(GLOB leftovers "${work}/*.bennu-build.tmp*"
                           "${work}/*.bennu-native.tmp*")
  if(leftovers)
    message(FATAL_ERROR "${context}: temporary artifacts remain: ${leftovers}")
  endif()
endfunction()

function(assert_failure_preserved context actual_exit actual_stdout actual_stderr
                                  expected_regex expected_output)
  if(EXISTS "${output}")
    file(READ "${output}" actual_output)
  else()
    set(actual_output "<missing>")
  endif()
  if("${actual_exit}" STREQUAL "0" OR NOT "${actual_stdout}" STREQUAL "" OR
     NOT "${actual_stderr}" MATCHES "${expected_regex}" OR
     NOT "${actual_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR
      "${context}: failure contract mismatch\nexit: ${actual_exit}\n"
      "stdout: [${actual_stdout}]\nstderr: [${actual_stderr}]\n"
      "output: [${actual_output}]")
  endif()
  assert_clean("${context}")
endfunction()

# Explicit --cc wins over a nonempty CC and preserves every argument boundary.
file(REMOVE "${trace}" "${sentinel}" "${output}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "CC=compiler that must not run"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
          --cc "${fake_copy}"
  WORKING_DIRECTORY "${work}"
  RESULT_VARIABLE explicit_exit
  OUTPUT_VARIABLE explicit_stdout
  ERROR_VARIABLE explicit_stderr
)
if(NOT "${explicit_exit}" STREQUAL "0" OR NOT explicit_stdout STREQUAL "" OR
   NOT explicit_stderr STREQUAL "" OR NOT EXISTS "${output}" OR
   NOT EXISTS "${trace}" OR EXISTS "${sentinel}")
  message(FATAL_ERROR
    "explicit compiler safety/precedence failed\nexit: ${explicit_exit}\n"
    "stdout: [${explicit_stdout}]\nstderr: [${explicit_stderr}]")
endif()
file(READ "${trace}" explicit_trace)
if(WIN32)
  if(NOT explicit_trace MATCHES "/std:c11" OR
     NOT explicit_trace MATCHES "/Fe:.*native with spaces .*\\$\\(touch sentinel\\).*bennu-build.tmp.*program.exe")
    message(FATAL_ERROR "MSVC fake compiler argument boundaries mismatch: ${explicit_trace}")
  endif()
else()
  if(NOT explicit_trace MATCHES "-std=c11" OR
     NOT explicit_trace MATCHES "native with spaces .*\\$\\(touch sentinel\\).*bennu-build.tmp.*program")
    message(FATAL_ERROR "GCC fake compiler argument boundaries mismatch: ${explicit_trace}")
  endif()
endif()
assert_clean("explicit compiler success")

# Relative executable paths are resolved from Bennu's launch directory, not the
# isolated compiler working directory.
file(RELATIVE_PATH relative_fake "${work}" "${fake_copy}")
if(NOT WIN32)
  set(relative_fake "./${relative_fake}")
endif()
file(REMOVE "${trace}" "${output}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=CC
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
          --cc "${relative_fake}"
  WORKING_DIRECTORY "${work}"
  RESULT_VARIABLE relative_exit OUTPUT_VARIABLE relative_stdout
  ERROR_VARIABLE relative_stderr)
if(NOT "${relative_exit}" STREQUAL "0" OR NOT relative_stdout STREQUAL "" OR
   NOT relative_stderr STREQUAL "" OR NOT EXISTS "${output}" OR
   NOT EXISTS "${trace}")
  message(FATAL_ERROR
    "relative explicit compiler path failed\nexit: ${relative_exit}\n"
    "stdout: [${relative_stdout}]\nstderr: [${relative_stderr}]")
endif()
assert_clean("relative explicit compiler success")

if(UNIX)
  # Bare compiler names found through a relative PATH entry retain caller-directory
  # lookup semantics after Bennu enters the isolated build directory.
  file(RELATIVE_PATH relative_path_entry "${work}" "${relative_path_directory}")
  foreach(selection explicit environment fallback)
    set(relative_output
        "${work}/relative PATH ${selection}${BENNU_EXECUTABLE_SUFFIX}")
    set(relative_trace "${work}/relative PATH ${selection} trace.txt")
    file(REMOVE "${relative_output}" "${relative_trace}" "${sentinel}")
    if(selection STREQUAL "explicit")
      execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=CC
                "PATH=${relative_path_entry}"
                "BENNU_FAKE_CC_MODE=success"
                "BENNU_FAKE_CC_TRACE=${relative_trace}"
                "${BENNU_EXECUTABLE}" build "${valid_source}"
                -o "${relative_output}" --cc relative-cc
        WORKING_DIRECTORY "${work}"
        RESULT_VARIABLE relative_path_exit
        OUTPUT_VARIABLE relative_path_stdout
        ERROR_VARIABLE relative_path_stderr)
    elseif(selection STREQUAL "environment")
      execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "PATH=${relative_path_entry}" "CC=relative-cc"
                "BENNU_FAKE_CC_MODE=success"
                "BENNU_FAKE_CC_TRACE=${relative_trace}"
                "${BENNU_EXECUTABLE}" build "${valid_source}"
                -o "${relative_output}"
        WORKING_DIRECTORY "${work}"
        RESULT_VARIABLE relative_path_exit
        OUTPUT_VARIABLE relative_path_stdout
        ERROR_VARIABLE relative_path_stderr)
    else()
      execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=CC
                "PATH=${relative_path_entry}"
                "BENNU_FAKE_CC_MODE=success"
                "BENNU_FAKE_CC_TRACE=${relative_trace}"
                "${BENNU_EXECUTABLE}" build "${valid_source}"
                -o "${relative_output}"
        WORKING_DIRECTORY "${work}"
        RESULT_VARIABLE relative_path_exit
        OUTPUT_VARIABLE relative_path_stdout
        ERROR_VARIABLE relative_path_stderr)
    endif()
    if(NOT "${relative_path_exit}" STREQUAL "0" OR
       NOT relative_path_stdout STREQUAL "" OR
       NOT relative_path_stderr STREQUAL "" OR
       NOT EXISTS "${relative_output}" OR NOT EXISTS "${relative_trace}" OR
       EXISTS "${sentinel}")
      message(FATAL_ERROR
        "relative PATH ${selection} compiler failed\nexit: ${relative_path_exit}\n"
        "stdout: [${relative_path_stdout}]\nstderr: [${relative_path_stderr}]")
    endif()
    file(READ "${relative_trace}" relative_path_trace)
    if(NOT relative_path_trace MATCHES "-std=c11" OR
       NOT relative_path_trace MATCHES
           "relative PATH ${selection}.*bennu-build.tmp.*program")
      message(FATAL_ERROR
        "relative PATH ${selection} argument boundaries mismatch: ${relative_path_trace}")
    endif()
    assert_clean("relative PATH ${selection} success")
  endforeach()
endif()

# CC is selected when --cc is absent.
file(REMOVE "${trace}" "${output}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "CC=${fake_copy}"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
  RESULT_VARIABLE cc_exit OUTPUT_VARIABLE cc_stdout ERROR_VARIABLE cc_stderr)
if(NOT "${cc_exit}" STREQUAL "0" OR NOT cc_stdout STREQUAL "" OR
   NOT cc_stderr STREQUAL "" OR NOT EXISTS "${trace}")
  message(FATAL_ERROR "CC compiler selection failed: ${cc_stderr}")
endif()
assert_clean("CC success")

# An occupied publication candidate is never overwritten or removed.
set(occupied_candidate "${output}.bennu-native.tmp")
file(WRITE "${occupied_candidate}" "occupied candidate\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "BENNU_FAKE_CC_MODE=success" "CC=${fake_copy}"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
  RESULT_VARIABLE collision_exit OUTPUT_VARIABLE collision_stdout
  ERROR_VARIABLE collision_stderr)
file(READ "${occupied_candidate}" occupied_contents)
file(GLOB numbered_candidates "${output}.bennu-native.tmp.*")
if(NOT "${collision_exit}" STREQUAL "0" OR NOT collision_stdout STREQUAL "" OR
   NOT collision_stderr STREQUAL "" OR
   NOT occupied_contents STREQUAL "occupied candidate\n" OR numbered_candidates)
  message(FATAL_ERROR "publication candidate collision handling failed")
endif()
file(REMOVE "${occupied_candidate}")
assert_clean("publication candidate collision")

# With --cc and CC absent, the platform fallback search selects its PATH candidate.
file(REMOVE "${trace}" "${output}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=CC
          "PATH=${fake_path_directory}"
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
  RESULT_VARIABLE fallback_exit OUTPUT_VARIABLE fallback_stdout
  ERROR_VARIABLE fallback_stderr)
if(NOT "${fallback_exit}" STREQUAL "0" OR NOT fallback_stdout STREQUAL "" OR
   NOT fallback_stderr STREQUAL "" OR NOT EXISTS "${trace}")
  message(FATAL_ERROR "platform fallback selection failed: ${fallback_stderr}")
endif()
assert_clean("fallback success")

# A selected explicit compiler failure is final; CC is not consulted.
file(WRITE "${output}" "sentinel bytes\n")
file(REMOVE "${trace}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "CC=${fake_copy}"
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
          --cc "${work}/missing explicit compiler"
  RESULT_VARIABLE missing_explicit_exit OUTPUT_VARIABLE missing_explicit_stdout
  ERROR_VARIABLE missing_explicit_stderr)
assert_failure_preserved("missing explicit" "${missing_explicit_exit}"
  "${missing_explicit_stdout}" "${missing_explicit_stderr}"
  "selected from --cc.*could not be started" "sentinel bytes\n")
if(EXISTS "${trace}")
  message(FATAL_ERROR "missing explicit compiler silently consulted CC")
endif()

# An invalid CC is final; fallback is not consulted.
file(REMOVE "${trace}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${fake_path_directory}"
          "CC=${work}/missing CC compiler"
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
  RESULT_VARIABLE missing_cc_exit OUTPUT_VARIABLE missing_cc_stdout
  ERROR_VARIABLE missing_cc_stderr)
assert_failure_preserved("missing CC" "${missing_cc_exit}" "${missing_cc_stdout}"
  "${missing_cc_stderr}" "selected from CC.*could not be started"
  "sentinel bytes\n")
if(EXISTS "${trace}")
  message(FATAL_ERROR "invalid CC silently consulted fallback")
endif()

# Missing fallback discovery is distinct from a selected process-start failure.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=CC "PATH=${empty_path}"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
  RESULT_VARIABLE discovery_exit OUTPUT_VARIABLE discovery_stdout
  ERROR_VARIABLE discovery_stderr)
assert_failure_preserved("missing discovery" "${discovery_exit}"
  "${discovery_stdout}" "${discovery_stderr}"
  "no C compiler found by platform fallback" "sentinel bytes\n")

# Nonzero, termination, and success-without-output retain the sentinel.
foreach(mode fail terminate no-output)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "BENNU_FAKE_CC_MODE=${mode}" "BENNU_FAKE_CC_TRACE=${trace}"
            "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
            --cc "${fake_copy}"
    RESULT_VARIABLE failure_exit OUTPUT_VARIABLE failure_stdout
    ERROR_VARIABLE failure_stderr)
  if(mode STREQUAL "fail")
    set(expected "selected from --cc.*exited with status 23.*fake compiler diagnostic")
  elseif(mode STREQUAL "terminate")
    set(expected "selected from --cc.*was terminated")
  else()
    set(expected "selected from --cc.*reported success without producing usable native output")
  endif()
  assert_failure_preserved("compiler ${mode}" "${failure_exit}"
    "${failure_stdout}" "${failure_stderr}" "${expected}" "sentinel bytes\n")
endforeach()

if(UNIX)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "BENNU_FAKE_CC_MODE=non-executable-output"
            "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
            --cc "${fake_copy}"
    RESULT_VARIABLE non_executable_output_exit
    OUTPUT_VARIABLE non_executable_output_stdout
    ERROR_VARIABLE non_executable_output_stderr)
  assert_failure_preserved("compiler non-executable output"
    "${non_executable_output_exit}" "${non_executable_output_stdout}"
    "${non_executable_output_stderr}"
    "reported success without producing usable native output" "sentinel bytes\n")
endif()

# Invalid Bennu source fails before any compiler process is started.
file(REMOVE "${trace}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${invalid_source}" -o "${output}"
          --cc "${fake_copy}"
  RESULT_VARIABLE invalid_exit OUTPUT_VARIABLE invalid_stdout
  ERROR_VARIABLE invalid_stderr)
assert_failure_preserved("invalid source" "${invalid_exit}" "${invalid_stdout}"
  "${invalid_stderr}" "invalid source.bennu:2:1: unknown name"
  "sentinel bytes\n")
if(EXISTS "${trace}")
  message(FATAL_ERROR "invalid source invoked the compiler")
endif()

# Shell-fragment CC text remains one executable name and cannot create a sentinel.
file(REMOVE "${sentinel}" "${trace}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "CC=${fake_copy}; & touch sentinel"
          "BENNU_FAKE_CC_MODE=success" "BENNU_FAKE_CC_TRACE=${trace}"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
  WORKING_DIRECTORY "${work}"
  RESULT_VARIABLE inert_exit OUTPUT_VARIABLE inert_stdout ERROR_VARIABLE inert_stderr)
assert_failure_preserved("inert CC shell text" "${inert_exit}" "${inert_stdout}"
  "${inert_stderr}" "selected from CC.*could not be started" "sentinel bytes\n")
if(EXISTS "${sentinel}" OR EXISTS "${trace}")
  message(FATAL_ERROR "CC shell fragment was interpreted")
endif()

# New-output failures do not leave a misleading executable.
file(REMOVE "${output}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BENNU_FAKE_CC_MODE=fail"
          "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
          --cc "${fake_copy}"
  RESULT_VARIABLE new_failure_exit OUTPUT_VARIABLE new_failure_stdout
  ERROR_VARIABLE new_failure_stderr)
if("${new_failure_exit}" STREQUAL "0" OR EXISTS "${output}" OR
   NOT new_failure_stderr MATCHES "exited with status 23")
  message(FATAL_ERROR "failed build left misleading new output")
endif()
assert_clean("new output failure")

# Output lifecycle failures neither replace a sentinel nor leave staged artifacts.
set(missing_parent_output
    "${work}/missing output parent/native${BENNU_EXECUTABLE_SUFFIX}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BENNU_FAKE_CC_MODE=success"
          "${BENNU_EXECUTABLE}" build "${valid_source}"
          -o "${missing_parent_output}" --cc "${fake_copy}"
  RESULT_VARIABLE parent_exit OUTPUT_VARIABLE parent_stdout
  ERROR_VARIABLE parent_stderr)
if("${parent_exit}" STREQUAL "0" OR EXISTS "${missing_parent_output}" OR
   NOT parent_stderr MATCHES "unable to create isolated build temporary directory")
  message(FATAL_ERROR "missing output parent lifecycle failure mismatch")
endif()
assert_clean("missing output parent")

set(directory_output "${work}/existing output directory")
file(MAKE_DIRECTORY "${directory_output}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BENNU_FAKE_CC_MODE=success"
          "${BENNU_EXECUTABLE}" build "${valid_source}"
          -o "${directory_output}" --cc "${fake_copy}"
  RESULT_VARIABLE directory_exit OUTPUT_VARIABLE directory_stdout
  ERROR_VARIABLE directory_stderr)
if("${directory_exit}" STREQUAL "0" OR NOT IS_DIRECTORY "${directory_output}" OR
   NOT directory_stderr MATCHES "unable to replace native output")
  message(FATAL_ERROR "directory output lifecycle failure mismatch")
endif()
assert_clean("directory output")

if(UNIX)
  set(non_executable "${work}/non executable compiler")
  configure_file("${BENNU_FAKE_COMPILER}" "${non_executable}" COPYONLY)
  execute_process(COMMAND chmod 600 "${non_executable}")
  file(WRITE "${output}" "sentinel bytes\n")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CC=${fake_copy}"
            "${BENNU_EXECUTABLE}" build "${valid_source}" -o "${output}"
            --cc "${non_executable}"
    RESULT_VARIABLE permission_exit OUTPUT_VARIABLE permission_stdout
    ERROR_VARIABLE permission_stderr)
  assert_failure_preserved("non-executable explicit compiler" "${permission_exit}"
    "${permission_stdout}" "${permission_stderr}"
    "selected from --cc.*could not be started" "sentinel bytes\n")
endif()

file(REMOVE_RECURSE "${work}")
