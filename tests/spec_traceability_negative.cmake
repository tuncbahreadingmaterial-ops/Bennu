if(NOT DEFINED BENNU_SOURCE_DIR OR NOT DEFINED BENNU_BINARY_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR and BENNU_BINARY_DIR are required")
endif()

set(mutation_root "${BENNU_BINARY_DIR}/spec-traceability-bounded-once")
file(REMOVE_RECURSE "${mutation_root}")
file(MAKE_DIRECTORY "${mutation_root}/tests")

set(traceability_record "${BENNU_SOURCE_DIR}/tests/spec-traceability.tsv")
set(traceability_validator "${BENNU_SOURCE_DIR}/tests/spec_traceability.cmake")
file(COPY "${traceability_record}" DESTINATION "${mutation_root}/tests")
file(COPY "${BENNU_SOURCE_DIR}/tests/spec_traceability.cmake"
  DESTINATION "${mutation_root}/tests")
file(COPY "${BENNU_SOURCE_DIR}/tests/required_tuple_traceability.cmake"
  DESTINATION "${mutation_root}/tests")
file(COPY "${BENNU_SOURCE_DIR}/CMakeLists.txt"
  DESTINATION "${mutation_root}")
file(STRINGS "${traceability_record}" traceability_rows)
foreach(row IN LISTS traceability_rows)
  if(row STREQUAL "" OR row MATCHES "^#")
    continue()
  endif()
  string(REPLACE "|" ";" fields "${row}")
  list(LENGTH fields field_count)
  if(NOT field_count EQUAL 3)
    message(FATAL_ERROR "malformed traceability row: ${row}")
  endif()
  list(GET fields 1 relative_source)
  set(source "${BENNU_SOURCE_DIR}/${relative_source}")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "isolated mutation input is missing: ${relative_source}")
  endif()
  get_filename_component(destination "${mutation_root}/${relative_source}"
                         DIRECTORY)
  file(MAKE_DIRECTORY "${destination}")
  file(COPY "${source}" DESTINATION "${destination}")
endforeach()

set(contract "${mutation_root}/tests/public_resource_contract.cmake")
file(READ "${contract}" contract_text)
set(reset_loop
  "# TEST-ID: PUBLIC-RESOURCE-RESET\nforeach(bounded_reset_iteration RANGE 1 2)")
set(mutated_reset_loop
  "# TEST-ID: PUBLIC-RESOURCE-RESET\nforeach(bounded_reset_iteration RANGE 1 1)")
string(REPLACE "${reset_loop}" "" contract_without_reset_loop "${contract_text}")
string(LENGTH "${contract_text}" contract_length)
string(LENGTH "${contract_without_reset_loop}" stripped_contract_length)
string(LENGTH "${reset_loop}" reset_loop_length)
math(EXPR reset_loop_count
  "(${contract_length} - ${stripped_contract_length}) / ${reset_loop_length}")
if(NOT reset_loop_count EQUAL 1)
  message(FATAL_ERROR
    "bounded-once mutation target count is ${reset_loop_count}, expected 1")
endif()
string(REPLACE "${reset_loop}" "${mutated_reset_loop}" contract_text
               "${contract_text}")
string(FIND "${contract_text}" "${reset_loop}" original_loop_at)
string(FIND "${contract_text}" "${mutated_reset_loop}" mutated_loop_at)
if(NOT original_loop_at EQUAL -1 OR mutated_loop_at EQUAL -1)
  message(FATAL_ERROR "bounded-once mutation replacement was not applied")
endif()
file(WRITE "${contract}" "${contract_text}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DBENNU_SOURCE_DIR=${mutation_root}"
          "-DBENNU_BINARY_DIR=${BENNU_BINARY_DIR}"
          -P "${mutation_root}/tests/spec_traceability.cmake"
  RESULT_VARIABLE mutation_exit OUTPUT_VARIABLE mutation_stdout
  ERROR_VARIABLE mutation_stderr)
if("${mutation_exit}" STREQUAL "0")
  message(FATAL_ERROR
    "bounded-once public resource mutation left spec traceability green\n"
    "stdout: [${mutation_stdout}]\nstderr: [${mutation_stderr}]")
endif()
if(NOT mutation_stderr MATCHES
       "bounded emitted/native two-invocation reset" OR
   NOT mutation_stderr MATCHES "assertions are missing")
  message(FATAL_ERROR
    "bounded-once mutation failed for an unexpected reason\n"
    "exit: ${mutation_exit}\nstdout: [${mutation_stdout}]\n"
    "stderr: [${mutation_stderr}]")
endif()

file(REMOVE_RECURSE "${mutation_root}")
