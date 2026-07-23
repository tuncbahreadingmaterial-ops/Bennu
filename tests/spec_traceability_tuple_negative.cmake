if(NOT DEFINED BENNU_SOURCE_DIR OR NOT DEFINED BENNU_BINARY_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR and BENNU_BINARY_DIR are required")
endif()

include("${BENNU_SOURCE_DIR}/tests/required_tuple_traceability.cmake")
set(traceability_record "${BENNU_SOURCE_DIR}/tests/spec-traceability.tsv")
set(traceability_validator "${BENNU_SOURCE_DIR}/tests/spec_traceability.cmake")
file(STRINGS "${traceability_record}" traceability_rows)

function(copy_traceability_inputs mutation_root)
  file(REMOVE_RECURSE "${mutation_root}")
  file(MAKE_DIRECTORY "${mutation_root}/tests")
  file(COPY "${traceability_record}" "${traceability_validator}"
       "${BENNU_SOURCE_DIR}/tests/required_tuple_traceability.cmake"
       DESTINATION "${mutation_root}/tests")
  foreach(row IN LISTS traceability_rows)
    if(row STREQUAL "" OR row MATCHES "^#")
      continue()
    endif()
    string(REPLACE "|" ";" fields "${row}")
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
  file(COPY "${BENNU_SOURCE_DIR}/CMakeLists.txt" DESTINATION "${mutation_root}")
endfunction()

function(expect_validator_failure mutation_root expected_fragment mutation_name)
  if(ARGC GREATER 3)
    set(validation_binary_dir "${ARGV3}")
  else()
    set(validation_binary_dir "${BENNU_BINARY_DIR}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DBENNU_SOURCE_DIR=${mutation_root}"
            "-DBENNU_BINARY_DIR=${validation_binary_dir}"
            -P "${mutation_root}/tests/spec_traceability.cmake"
    RESULT_VARIABLE mutation_exit OUTPUT_VARIABLE mutation_stdout
    ERROR_VARIABLE mutation_stderr)
  if("${mutation_exit}" STREQUAL "0")
    message(FATAL_ERROR
      "${mutation_name} left spec traceability green\n"
      "stdout: [${mutation_stdout}]\nstderr: [${mutation_stderr}]")
  endif()
  string(FIND "${mutation_stderr}" "${expected_fragment}" fragment_at)
  if(fragment_at EQUAL -1)
    message(FATAL_ERROR
      "${mutation_name} failed for an unexpected reason\n"
      "exit: ${mutation_exit}\nstdout: [${mutation_stdout}]\n"
      "stderr: [${mutation_stderr}]\n"
      "expected fragment: [${expected_fragment}]")
  endif()
endfunction()

foreach(required_row IN LISTS BENNU_REQUIRED_TUP_TRACEABILITY_ROWS)
  string(REPLACE "|" ";" required_fields "${required_row}")
  list(GET required_fields 2 required_identifier)
  set(mutation_root
      "${BENNU_BINARY_DIR}/spec-traceability-delete-${required_identifier}")
  copy_traceability_inputs("${mutation_root}")
  set(record "${mutation_root}/tests/spec-traceability.tsv")
  file(READ "${record}" record_text)
  string(REPLACE "${required_row}\n" "" mutated_text "${record_text}")
  if(mutated_text STREQUAL record_text)
    message(FATAL_ERROR
      "deletion target is absent or not exact: ${required_identifier}")
  endif()
  file(WRITE "${record}" "${mutated_text}")
  expect_validator_failure("${mutation_root}"
    "missing required traceability row: ${required_identifier}"
    "deleting ${required_identifier}")
  file(REMOVE_RECURSE "${mutation_root}")
endforeach()

list(GET BENNU_REQUIRED_TUP_TRACEABILITY_ROWS 0 mapped_row)
string(REPLACE "|" ";" mapped_fields "${mapped_row}")
list(GET mapped_fields 2 mapped_identifier)
set(mapping_root "${BENNU_BINARY_DIR}/spec-traceability-map-${mapped_identifier}")
copy_traceability_inputs("${mapping_root}")
set(mapping_record "${mapping_root}/tests/spec-traceability.tsv")
file(READ "${mapping_record}" mapping_text)
string(REPLACE "${mapped_row}"
  "BENNU-SPEC-0006 section 4 structural type arenas|tests/tuple_foundation_conformance.cpp|${mapped_identifier}"
  mapping_text "${mapping_text}")
file(WRITE "${mapping_record}" "${mapping_text}")
expect_validator_failure("${mapping_root}"
  "missing required traceability row: ${mapped_identifier}"
  "changing ${mapped_identifier} mapping")
file(REMOVE_RECURSE "${mapping_root}")

set(topology_root "${BENNU_BINARY_DIR}/spec-traceability-registration")
copy_traceability_inputs("${topology_root}")
set(topology_binary "${topology_root}/configured")
file(MAKE_DIRECTORY "${topology_binary}")
set(topology_source "${BENNU_BINARY_DIR}/CTestTestfile.cmake")
set(topology_mutation "${topology_binary}/CTestTestfile.cmake")
file(READ "${topology_source}" topology_text)
string(REGEX REPLACE
  "add_test\\([^\r\n]*public\\.api_resource_matrix[^\r\n]*[\r]?[\n]" ""
  mutated_topology "${topology_text}")
if(mutated_topology STREQUAL topology_text)
  message(FATAL_ERROR "public API registration mutation did not change topology")
endif()
file(WRITE "${topology_mutation}" "${mutated_topology}")
expect_validator_failure("${topology_root}"
  "required CTest registration count for public.api_resource_matrix: 0"
  "deleting public.api_resource_matrix registration" "${topology_binary}")
file(REMOVE_RECURSE "${topology_root}")
