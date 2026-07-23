# TEST-ID: DOCS-SMOKE
foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

file(READ "${BENNU_SOURCE_DIR}/README.md" readme)
file(READ "${BENNU_SOURCE_DIR}/VERSION" product_version)
string(REGEX REPLACE "\n$" "" product_version "${product_version}")
foreach(required_text IN ITEMS
    "inc" "add" "equals" "not" "iota"
    "examples/rewrite.bennu" "trusted-local-v1"
    "run example.bennu --" "bennu_argument_error"
    "Deliberate differences from Anka"
    "`${product_version}`;" "VERSION does not authorize a release"
    "CLOSED NOT_PLANNED by the owner"
    "not Authenticode or Apple Developer ID/notarization"
    "github-artifact-attestation"
    "source_commit" "archive SHA-256" "executable SHA-256"
    "gh release download" "gh attestation verify"
    "Get-FileHash" "shasum -a 256" "sha256sum"
    "v0.1.0 predates this contract")
  string(FIND "${readme}" "${required_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "README is missing shipped rewrite text: ${required_text}")
  endif()
endforeach()

execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run
          "${BENNU_SOURCE_DIR}/examples/rewrite.bennu"
  RESULT_VARIABLE run_exit OUTPUT_VARIABLE run_stdout ERROR_VARIABLE run_stderr)
file(READ "${BENNU_SOURCE_DIR}/tests/fixtures/rewrite-example.out" expected)
string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")
if(NOT "${run_exit}" STREQUAL "0" OR NOT run_stderr STREQUAL "" OR
   NOT run_stdout STREQUAL expected)
  message(FATAL_ERROR
    "documented runner journey failed\nexit: ${run_exit}\n"
    "stdout: [${run_stdout}]\nstderr: [${run_stderr}]")
endif()
