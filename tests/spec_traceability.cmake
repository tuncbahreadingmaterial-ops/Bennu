if(NOT DEFINED BENNU_SOURCE_DIR OR NOT DEFINED BENNU_BINARY_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR and BENNU_BINARY_DIR are required")
endif()

set(record "${BENNU_SOURCE_DIR}/tests/spec-traceability.tsv")
file(STRINGS "${record}" rows)
include("${BENNU_SOURCE_DIR}/tests/required_tuple_traceability.cmake")
file(READ "${BENNU_SOURCE_DIR}/CMakeLists.txt" cmake_source)
set(ctest_topology "${BENNU_BINARY_DIR}/CTestTestfile.cmake")
if(NOT EXISTS "${ctest_topology}")
  message(FATAL_ERROR "configured CTest topology is required: ${ctest_topology}")
endif()
file(READ "${ctest_topology}" ctest_source)
foreach(expected_row IN LISTS BENNU_REQUIRED_TUP_TRACEABILITY_ROWS)
  string(REPLACE "|" ";" expected_fields "${expected_row}")
  list(GET expected_fields 1 expected_source)
  list(GET expected_fields 2 expected_identifier)
  set(expected_count 0)
  foreach(candidate_row IN LISTS rows)
    if(candidate_row STREQUAL expected_row)
      math(EXPR expected_count "${expected_count} + 1")
    endif()
  endforeach()
  if(NOT expected_count EQUAL 1)
    message(FATAL_ERROR
      "missing required traceability row: ${expected_identifier}")
  endif()
  string(FIND "${cmake_source}" "${expected_source}" registration_at)
  if(registration_at EQUAL -1)
    message(FATAL_ERROR
      "required traceability source is not registered: ${expected_source}")
  endif()
  if(expected_source STREQUAL "tests/tuple_foundation_conformance.cpp")
    set(expected_test "unit.doctest")
  elseif(expected_source STREQUAL "tests/public_resource_contract.cmake")
    set(expected_test "public.api_resource_matrix")
  else()
    message(FATAL_ERROR
      "required tuple source has no CTest topology rule: ${expected_source}")
  endif()
  string(REGEX MATCHALL
    "add_test\\([^\r\n]*${expected_test}" topology_matches "${ctest_source}")
  list(LENGTH topology_matches topology_count)
  if(NOT topology_count EQUAL 1)
    message(FATAL_ERROR
      "required CTest registration count for ${expected_test}: ${topology_count}")
  endif()
endforeach()

set(requirement_count 0)
foreach(row IN LISTS rows)
  if(row STREQUAL "" OR row MATCHES "^#")
    continue()
  endif()
  string(REPLACE "|" ";" fields "${row}")
  list(LENGTH fields field_count)
  if(NOT field_count EQUAL 3)
    message(FATAL_ERROR "malformed traceability row: ${row}")
  endif()
  list(GET fields 0 requirement)
  list(GET fields 1 relative_source)
  list(GET fields 2 identifier)
  set(source "${BENNU_SOURCE_DIR}/${relative_source}")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "${requirement}: missing test source ${relative_source}")
  endif()
  file(READ "${source}" source_text)
  if(relative_source MATCHES "\\.cmake$")
    set(identifier_needle "# TEST-ID: ${identifier}\n")
  elseif(relative_source MATCHES "\\.cpp$")
    set(identifier_needle "TEST_CASE(\"${identifier}\")")
  else()
    message(FATAL_ERROR
      "${requirement}: traceability source is not an executable test: ${relative_source}")
  endif()
  string(FIND "${source_text}" "${identifier_needle}" identifier_at)
  if(identifier_at EQUAL -1)
    message(FATAL_ERROR
      "${requirement}: '${identifier}' is not an exact test identifier in ${relative_source}")
  endif()
  string(REPLACE "${identifier_needle}" "" source_without_identifier
                 "${source_text}")
  string(LENGTH "${source_text}" source_length)
  string(LENGTH "${source_without_identifier}" stripped_source_length)
  string(LENGTH "${identifier_needle}" identifier_length)
  math(EXPR identifier_count
    "(${source_length} - ${stripped_source_length}) / ${identifier_length}")
  if(NOT identifier_count EQUAL 1)
    message(FATAL_ERROR
      "${requirement}: duplicate test identifier '${identifier}' in ${relative_source}")
  endif()
  if(identifier STREQUAL "PUBLIC-RESOURCE-BOUNDED-REFUSAL")
    foreach(required_assertion
        "NOT run_stdout STREQUAL \"\""
        "NOT run_stderr STREQUAL expected_stderr"
        "check_profile_refusal(refusal-emitted \"\${refusal_emitted}\""
        "check_profile_refusal(refusal-native \"\${refusal_native}\"")
      string(FIND "${source_text}" "${required_assertion}" assertion_at)
      if(assertion_at EQUAL -1)
        message(FATAL_ERROR
          "${requirement}: bounded emitted/native refusal assertion is missing: ${required_assertion}")
      endif()
    endforeach()
  elseif(identifier STREQUAL "PUBLIC-RESOURCE-RESET")
    set(expected_reset_block [=[# TEST-ID: PUBLIC-RESOURCE-RESET
foreach(bounded_reset_iteration RANGE 1 2)
  check_success(profile-emitted "${profile_emitted}" "(2)\n(2)\n"
                "${bounded_reset_iteration}")
  check_success(profile-native "${profile_native}" "(2)\n(2)\n"
                "${bounded_reset_iteration}")
  check_profile_refusal(refusal-emitted "${refusal_emitted}"
                        "${bounded_reset_iteration}")
  check_profile_refusal(refusal-native "${refusal_native}"
                        "${bounded_reset_iteration}")
endforeach()]=])
    string(FIND "${source_text}" "${expected_reset_block}" reset_block_at)
    if(reset_block_at EQUAL -1)
      message(FATAL_ERROR
        "${requirement}: bounded emitted/native two-invocation reset assertions are missing")
    endif()
  endif()
  math(EXPR requirement_count "${requirement_count} + 1")
endforeach()

if(requirement_count LESS 30)
  message(FATAL_ERROR
    "traceability record is incomplete: only ${requirement_count} requirements")
endif()
