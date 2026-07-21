if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE active_files
  LIST_DIRECTORIES false
  "${BENNU_SOURCE_DIR}/include/*"
  "${BENNU_SOURCE_DIR}/src/*"
  "${BENNU_SOURCE_DIR}/examples/*"
  "${BENNU_SOURCE_DIR}/tests/*"
  "${BENNU_SOURCE_DIR}/.github/*")
file(GLOB top_level_hidden_configuration
  LIST_DIRECTORIES false
  "${BENNU_SOURCE_DIR}/.*")
list(APPEND active_files ${top_level_hidden_configuration})
list(APPEND active_files "${BENNU_SOURCE_DIR}/README.md")
list(REMOVE_DUPLICATES active_files)
string(CONCAT obsolete_constructor "io" "ata")
foreach(path IN LISTS active_files)
  if(path STREQUAL "${BENNU_SOURCE_DIR}/.git" OR
     IS_DIRECTORY "${path}" OR
     path MATCHES "/tests/obsolete_surface_contract\\.cmake$")
    continue()
  endif()
  file(READ "${path}" text)
  string(REPLACE
    "0f31967e0a70b424f4201133a54ae7cd8aa5d659:examples/level1.bennu"
    ""
    current_tree_text
    "${text}")
  foreach(obsolete IN ITEMS
      "${obsolete_constructor}"
      "bootstrap_vector_bytes"
      "TokenKind::inc"
      "ExpressionKind::inc"
      "apply_ioata"
      "ioata_element_limit"
      "level1-example"
      "examples/level1.bennu"
      "bennu_print_array(int64_t count)")
    string(FIND "${current_tree_text}" "${obsolete}" obsolete_at)
    if(NOT obsolete_at EQUAL -1)
      message(FATAL_ERROR "obsolete public surface '${obsolete}' remains in ${path}")
    endif()
  endforeach()
endforeach()

foreach(removed IN ITEMS
    "src/evaluator.cpp"
    "src/c_emitter.cpp"
    "src/rewrite_backend.cpp"
    "src/rewrite_backend.hpp"
    "tests/rewrite_backend_test_driver.cpp"
    "tests/rewrite_backend_contract.cmake"
    "examples/level1.bennu"
    "tests/fixtures/level1-example.bennu"
    "tests/fixtures/level1-example.c"
    "tests/fixtures/level1-example.out")
  if(EXISTS "${BENNU_SOURCE_DIR}/${removed}")
    message(FATAL_ERROR "obsolete file remains: ${removed}")
  endif()
endforeach()
