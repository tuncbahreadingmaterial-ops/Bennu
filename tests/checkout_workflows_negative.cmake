if(NOT DEFINED BENNU_SOURCE_DIR OR NOT DEFINED BENNU_BINARY_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR and BENNU_BINARY_DIR are required")
endif()

set(workflow_root "${BENNU_SOURCE_DIR}/.github/workflows")
set(checkout_contract "${BENNU_SOURCE_DIR}/tests/checkout_workflows.cmake")
set(mutation_root "${BENNU_BINARY_DIR}/checkout-workflow-negative")

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DBENNU_SOURCE_DIR=${BENNU_SOURCE_DIR}"
    "-DBENNU_CHECKOUT_WORKFLOW_ROOT=${workflow_root}"
    -P "${checkout_contract}"
  RESULT_VARIABLE canonical_exit
  OUTPUT_VARIABLE canonical_stdout
  ERROR_VARIABLE canonical_stderr)
if(NOT canonical_exit EQUAL 0)
  message(FATAL_ERROR
    "canonical checkout workflows failed before negative mutations (${canonical_exit})\n"
    "stdout: [${canonical_stdout}]\nstderr: [${canonical_stderr}]")
endif()

function(expect_rejected name workflow_file original replacement expected_error)
  file(REMOVE_RECURSE "${mutation_root}")
  file(MAKE_DIRECTORY "${mutation_root}")
  file(COPY "${workflow_root}/" DESTINATION "${mutation_root}")

  set(mutated_path "${mutation_root}/${workflow_file}")
  file(READ "${mutated_path}" canonical_workflow)
  string(REPLACE "${original}" "" workflow_without_original
    "${canonical_workflow}")
  string(LENGTH "${canonical_workflow}" workflow_length)
  string(LENGTH "${workflow_without_original}" stripped_length)
  string(LENGTH "${original}" original_length)
  math(EXPR original_count
    "(${workflow_length} - ${stripped_length}) / ${original_length}")
  if(original_count LESS 1)
    message(FATAL_ERROR "${name}: mutation target is absent")
  endif()

  string(REPLACE "${original}" "${replacement}" mutated_workflow
    "${canonical_workflow}")
  if(mutated_workflow STREQUAL canonical_workflow)
    message(FATAL_ERROR "${name}: mutation was not applied")
  endif()
  file(WRITE "${mutated_path}" "${mutated_workflow}")

  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${BENNU_SOURCE_DIR}"
      "-DBENNU_CHECKOUT_WORKFLOW_ROOT=${mutation_root}"
      -P "${checkout_contract}"
    RESULT_VARIABLE mutation_exit
    OUTPUT_VARIABLE mutation_stdout
    ERROR_VARIABLE mutation_stderr)
  if(mutation_exit EQUAL 0)
    message(FATAL_ERROR
      "${name}: unsafe checkout mutation left the contract green")
  endif()
  if(NOT mutation_stderr MATCHES "${expected_error}")
    message(FATAL_ERROR
      "${name}: mutation failed for an unexpected reason\n"
      "exit: ${mutation_exit}\nstdout: [${mutation_stdout}]\n"
      "stderr: [${mutation_stderr}]")
  endif()
endfunction()

set(reviewed_pin
  "uses: actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09 # v5.1.0")

expect_rejected(
  deprecated_release_checkout
  release.yml
  "${reviewed_pin}"
  "uses: actions/checkout@34e114876b0b11c390a56381ad16ebd13914f8d5 # v4.3.1"
  "full-SHA pin")
expect_rejected(
  deprecated_future_release_checkout
  future-release.yml
  "${reviewed_pin}"
  "uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0"
  "full-SHA pin")
expect_rejected(
  mutable_checkout
  main.yml
  "${reviewed_pin}"
  "uses: actions/checkout@v5.1.0 # v5.1.0"
  "full-SHA pin")
expect_rejected(
  inaccurate_release_comment
  main.yml
  "${reviewed_pin}"
  "uses: actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09 # v5"
  "full-SHA pin")
expect_rejected(
  node_runtime_escape_hatch
  main.yml
  "permissions: {}"
  "permissions: {}\nenv:\n  ACTIONS_ALLOW_USE_UNSECURE_NODE_VERSION: true"
  "runtime-warning suppression")

file(REMOVE_RECURSE "${mutation_root}")
