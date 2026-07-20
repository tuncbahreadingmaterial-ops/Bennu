if(NOT DEFINED BENNU_EXECUTABLE OR
   NOT DEFINED BENNU_SOURCE_DIR OR
   NOT DEFINED BENNU_BINARY_DIR OR
   NOT DEFINED BENNU_BASH_EXECUTABLE)
  message(FATAL_ERROR "Linux package contract requires executable, source, binary, and Bash paths")
endif()

set(work_root "${BENNU_BINARY_DIR}/linux-package-contract")
set(archive "${work_root}/bennu-v0.1.0-linux-x64.tar.gz")
file(REMOVE_RECURSE "${work_root}")
file(MAKE_DIRECTORY "${work_root}")

execute_process(
  COMMAND
    "${BENNU_BASH_EXECUTABLE}"
    "${BENNU_SOURCE_DIR}/tools/release/package-linux.sh"
    "${BENNU_EXECUTABLE}"
    "${BENNU_SOURCE_DIR}/LICENSE"
    "${archive}"
  RESULT_VARIABLE package_status
  OUTPUT_VARIABLE package_stdout
  ERROR_VARIABLE package_stderr
)
if(NOT package_status EQUAL 0)
  message(FATAL_ERROR
    "Linux packaging failed (${package_status})\nstdout:\n${package_stdout}\nstderr:\n${package_stderr}")
endif()

execute_process(
  COMMAND
    "${BENNU_BASH_EXECUTABLE}"
    "${BENNU_SOURCE_DIR}/tools/release/verify-linux-package.sh"
    "${archive}"
    "${BENNU_SOURCE_DIR}/examples/level1.bennu"
    "${BENNU_SOURCE_DIR}/LICENSE"
  RESULT_VARIABLE verify_status
  OUTPUT_VARIABLE verify_stdout
  ERROR_VARIABLE verify_stderr
)
if(NOT verify_status EQUAL 0)
  message(FATAL_ERROR
    "Linux package execution failed (${verify_status})\nstdout:\n${verify_stdout}\nstderr:\n${verify_stderr}")
endif()

foreach(required_output IN ITEMS
    "Verified Linux archive"
    "Linux ELF compatibility policy passed"
    "Linux package journeys passed")
  string(FIND "${package_stdout}\n${verify_stdout}" "${required_output}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR
      "Linux package evidence is missing '${required_output}'\npackage:\n${package_stdout}\nverify:\n${verify_stdout}")
  endif()
endforeach()

set(failure_root "${work_root}/atomic-failure")
set(failure_archive "${failure_root}/bennu-v0.1.0-linux-x64-negative.tar.gz")
file(MAKE_DIRECTORY "${failure_root}")
configure_file(
  "${BENNU_SOURCE_DIR}/tools/release/package-linux.sh"
  "${failure_root}/package-linux.sh"
  COPYONLY
)
file(WRITE "${failure_root}/verify-linux-elf.py" [=[
import pathlib
import sys

calls_path = pathlib.Path(__file__).with_name("calls")
calls = int(calls_path.read_text()) + 1 if calls_path.exists() else 1
calls_path.write_text(str(calls))
if calls == 2:
    sys.exit(23)
]=])

execute_process(
  COMMAND
    "${BENNU_BASH_EXECUTABLE}"
    "${failure_root}/package-linux.sh"
    "${BENNU_EXECUTABLE}"
    "${BENNU_SOURCE_DIR}/LICENSE"
    "${failure_archive}"
  RESULT_VARIABLE failure_status
  OUTPUT_VARIABLE failure_stdout
  ERROR_VARIABLE failure_stderr
)
if(NOT failure_status EQUAL 23)
  message(FATAL_ERROR
    "controlled post-archive verification returned ${failure_status}, expected 23\nstdout:\n${failure_stdout}\nstderr:\n${failure_stderr}")
endif()
if(EXISTS "${failure_archive}")
  message(FATAL_ERROR
    "failed Linux packaging exposed the caller-visible archive: ${failure_archive}")
endif()
file(GLOB leaked_archives "${failure_root}/.bennu-v0.1.0-linux-x64-negative.tar.gz.tmp.*")
if(leaked_archives)
  message(FATAL_ERROR
    "failed Linux packaging left temporary archives: ${leaked_archives}")
endif()

file(REMOVE_RECURSE "${work_root}")
