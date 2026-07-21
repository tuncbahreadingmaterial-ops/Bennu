if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

set(record "${BENNU_SOURCE_DIR}/tests/spec-traceability.tsv")
file(STRINGS "${record}" rows)
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
        "foreach(invocation RANGE 1 2)"
        "NOT run_stdout STREQUAL \"\""
        "NOT run_stderr STREQUAL expected_stderr"
        "check_profile_refusal(refusal-emitted \"\${refusal_emitted}\")"
        "check_profile_refusal(refusal-native \"\${refusal_native}\")")
      string(FIND "${source_text}" "${required_assertion}" assertion_at)
      if(assertion_at EQUAL -1)
        message(FATAL_ERROR
          "${requirement}: bounded emitted/native refusal assertion is missing: ${required_assertion}")
      endif()
    endforeach()
  elseif(identifier STREQUAL "PUBLIC-RESOURCE-RESET")
    string(FIND "${source_text}"
      "foreach(invocation RANGE 1 2)" reset_loop_at)
    string(FIND "${source_text}"
      "# TEST-ID: PUBLIC-RESOURCE-RESET\ncheck_profile_refusal(refusal-emitted \"\${refusal_emitted}\")\ncheck_profile_refusal(refusal-native \"\${refusal_native}\")"
      reset_calls_at)
    if(reset_loop_at EQUAL -1 OR reset_calls_at EQUAL -1)
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
