if(NOT DEFINED BENNU_SOURCE_DIR OR NOT DEFINED BENNU_BINARY_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR and BENNU_BINARY_DIR are required")
endif()

set(main_workflow "${BENNU_SOURCE_DIR}/.github/workflows/main.yml")
set(main_contract "${BENNU_SOURCE_DIR}/tests/main_workflow.cmake")
set(mutation_root "${BENNU_BINARY_DIR}/main-workflow-negative")
file(REMOVE_RECURSE "${mutation_root}")
file(MAKE_DIRECTORY "${mutation_root}")
file(READ "${main_workflow}" canonical_workflow)

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DBENNU_SOURCE_DIR=${BENNU_SOURCE_DIR}"
    "-DBENNU_MAIN_WORKFLOW=${main_workflow}"
    -P "${main_contract}"
  RESULT_VARIABLE canonical_exit
  OUTPUT_VARIABLE canonical_stdout
  ERROR_VARIABLE canonical_stderr)
if(NOT canonical_exit EQUAL 0)
  message(FATAL_ERROR
    "canonical Main CI failed before negative mutations (${canonical_exit})\n"
    "stdout: [${canonical_stdout}]\nstderr: [${canonical_stderr}]")
endif()

function(expect_rejected name original replacement expected_error)
  string(REPLACE "${original}" "" workflow_without_original
    "${canonical_workflow}")
  string(LENGTH "${canonical_workflow}" workflow_length)
  string(LENGTH "${workflow_without_original}" stripped_length)
  string(LENGTH "${original}" original_length)
  math(EXPR original_count
    "(${workflow_length} - ${stripped_length}) / ${original_length}")
  if(NOT original_count EQUAL 1)
    message(FATAL_ERROR
      "${name}: mutation target count is ${original_count}, expected 1")
  endif()

  string(REPLACE "${original}" "${replacement}" mutated_workflow
    "${canonical_workflow}")
  if(mutated_workflow STREQUAL canonical_workflow)
    message(FATAL_ERROR "${name}: mutation was not applied")
  endif()

  set(mutated_path "${mutation_root}/${name}.yml")
  file(WRITE "${mutated_path}" "${mutated_workflow}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DBENNU_SOURCE_DIR=${BENNU_SOURCE_DIR}"
      "-DBENNU_MAIN_WORKFLOW=${mutated_path}"
      -P "${main_contract}"
    RESULT_VARIABLE mutation_exit
    OUTPUT_VARIABLE mutation_stdout
    ERROR_VARIABLE mutation_stderr)
  if(mutation_exit EQUAL 0)
    message(FATAL_ERROR
      "${name}: unsafe Main CI mutation left the contract green")
  endif()
  if(NOT mutation_stderr MATCHES "${expected_error}")
    message(FATAL_ERROR
      "${name}: mutation failed for an unexpected reason\n"
      "exit: ${mutation_exit}\nstdout: [${mutation_stdout}]\n"
      "stderr: [${mutation_stderr}]")
  endif()
endfunction()

expect_rejected(
  node20_checkout
  "uses: actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09 # v5.1.0"
  "uses: actions/checkout@34e114876b0b11c390a56381ad16ebd13914f8d5 # v4.3.1"
  "Node-24-native actions/checkout")
expect_rejected(
  mutable_checkout
  "uses: actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09 # v5.1.0"
  "uses: actions/checkout@v5.1.0"
  "Node-24-native actions/checkout")
expect_rejected(
  node20_msvc
  [=[uses: mlocati/setup-msvc@ade6aff3df872d66c12a63dcacdddf0041cb2693 # v1.3.1
        with:
          architecture: x64]=]
  [=[uses: ilammy/msvc-dev-cmd@0b201ec74fa43914dc39ae48a89fd1d8cb592756 # v1.13.0
        with:
          arch: x64]=]
  "Node-24-native setup-msvc")
expect_rejected(
  node20_escape_hatch
  "permissions: {}"
  [=[permissions: {}
env:
  ACTIONS_ALLOW_USE_UNSECURE_NODE_VERSION: true]=]
  "forbidden obsolete")

file(REMOVE_RECURSE "${mutation_root}")
