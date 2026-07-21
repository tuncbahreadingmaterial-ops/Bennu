if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

file(READ "${BENNU_SOURCE_DIR}/VERSION" actual_version_hex HEX)
set(expected_version_hex "302e322e300a")
if(NOT actual_version_hex STREQUAL expected_version_hex)
  message(FATAL_ERROR
    "VERSION must contain exact bytes '0.2.0\\n': expected ${expected_version_hex}, got ${actual_version_hex}")
endif()
