if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

if(DEFINED BENNU_CHECKOUT_WORKFLOW_ROOT)
  set(workflow_root "${BENNU_CHECKOUT_WORKFLOW_ROOT}")
else()
  set(workflow_root "${BENNU_SOURCE_DIR}/.github/workflows")
endif()

if(NOT IS_DIRECTORY "${workflow_root}")
  message(FATAL_ERROR "workflow directory is missing: ${workflow_root}")
endif()

file(GLOB workflow_files
  RELATIVE "${workflow_root}"
  "${workflow_root}/*.yml"
  "${workflow_root}/*.yaml")
list(SORT workflow_files)
set(expected_workflows
  future-release.yml
  main.yml
  release.yml)
if(NOT "${workflow_files}" STREQUAL "${expected_workflows}")
  message(FATAL_ERROR
    "tracked workflow set changed; expected [${expected_workflows}], found [${workflow_files}]")
endif()

set(checkout_pin
  "uses: actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09 # v5.1.0")

foreach(workflow_file IN LISTS workflow_files)
  file(READ "${workflow_root}/${workflow_file}" workflow_text)
  string(REGEX MATCHALL
    "uses:[ \t]+actions/checkout@[^\r\n]*"
    checkout_references
    "${workflow_text}")
  list(LENGTH checkout_references checkout_count)

  if(workflow_file STREQUAL "future-release.yml")
    set(expected_checkout_count 4)
  elseif(workflow_file STREQUAL "main.yml")
    set(expected_checkout_count 1)
  elseif(workflow_file STREQUAL "release.yml")
    set(expected_checkout_count 5)
  endif()

  if(NOT checkout_count EQUAL expected_checkout_count)
    message(FATAL_ERROR
      "${workflow_file} must contain ${expected_checkout_count} checkout references; found ${checkout_count}")
  endif()

  foreach(checkout_reference IN LISTS checkout_references)
    if(NOT checkout_reference STREQUAL checkout_pin)
      message(FATAL_ERROR
        "${workflow_file} must use only the reviewed Node-24-native actions/checkout v5.1.0 full-SHA pin; found: ${checkout_reference}")
    endif()
  endforeach()

  string(FIND "${workflow_text}"
    "ACTIONS_ALLOW_USE_UNSECURE_NODE_VERSION"
    suppression_at)
  if(NOT suppression_at EQUAL -1)
    message(FATAL_ERROR
      "${workflow_file} must not use Node runtime-warning suppression")
  endif()
endforeach()
