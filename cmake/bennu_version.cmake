if(NOT DEFINED BENNU_VERSION_FILE)
  message(FATAL_ERROR "BENNU_VERSION_FILE is required")
endif()

file(READ "${BENNU_VERSION_FILE}" bennu_version_file)
file(READ "${BENNU_VERSION_FILE}" bennu_version_hex HEX)
string(REGEX REPLACE "\n$" "" BENNU_VERSION "${bennu_version_file}")
string(HEX "${BENNU_VERSION}\n" bennu_expected_version_hex)
if(NOT bennu_version_hex STREQUAL bennu_expected_version_hex)
  message(FATAL_ERROR
    "VERSION must contain exactly one UTF-8 SemVer line terminated by LF"
  )
endif()
set(bennu_semver_pattern
  "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-([0-9A-Za-z-]+)(\\.[0-9A-Za-z-]+)*)?(\\+([0-9A-Za-z-]+)(\\.[0-9A-Za-z-]+)*)?$"
)
if(NOT BENNU_VERSION MATCHES "${bennu_semver_pattern}")
  message(FATAL_ERROR "VERSION is not valid SemVer: ${BENNU_VERSION}")
endif()
set(BENNU_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(BENNU_VERSION_MINOR "${CMAKE_MATCH_2}")
set(BENNU_VERSION_PATCH "${CMAKE_MATCH_3}")
foreach(component IN ITEMS
    BENNU_VERSION_MAJOR BENNU_VERSION_MINOR BENNU_VERSION_PATCH)
  if(${component} GREATER 65535)
    message(FATAL_ERROR
      "VERSION numeric core exceeds the Windows PE component limit: ${BENNU_VERSION}"
    )
  endif()
endforeach()
if(BENNU_VERSION MATCHES "^[^-+]+-([^+]+)")
  string(REPLACE "." ";" bennu_prerelease_identifiers "${CMAKE_MATCH_1}")
  foreach(identifier IN LISTS bennu_prerelease_identifiers)
    if(identifier MATCHES "^0[0-9]+$")
      message(FATAL_ERROR
        "VERSION has a numeric prerelease identifier with a leading zero: ${identifier}"
      )
    endif()
  endforeach()
endif()
