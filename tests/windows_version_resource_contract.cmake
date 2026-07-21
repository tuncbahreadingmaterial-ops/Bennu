if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

set(resource_template "${BENNU_SOURCE_DIR}/src/bennu-version.rc.in")
if(NOT EXISTS "${resource_template}")
  message(FATAL_ERROR "Windows VERSION-derived resource template is missing")
endif()
file(READ "${resource_template}" resource_text)
file(READ "${BENNU_SOURCE_DIR}/CMakeLists.txt" cmake_text)
file(READ "${BENNU_SOURCE_DIR}/tools/release/package-and-smoke.ps1" package_text)

function(require_text haystack expected description)
  string(FIND "${${haystack}}" "${expected}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "missing ${description}: ${expected}")
  endif()
endfunction()

require_text(resource_text
  "FILEVERSION @BENNU_VERSION_MAJOR@,@BENNU_VERSION_MINOR@,@BENNU_VERSION_PATCH@,0"
  "numeric FileVersion")
require_text(resource_text
  "PRODUCTVERSION @BENNU_VERSION_MAJOR@,@BENNU_VERSION_MINOR@,@BENNU_VERSION_PATCH@,0"
  "numeric ProductVersion")
foreach(pair IN ITEMS
    "FileDescription|Bennu data-oriented programming language"
    "FileVersion|@BENNU_VERSION@"
    "OriginalFilename|bennu.exe"
    "ProductName|Bennu"
    "ProductVersion|@BENNU_VERSION@")
  string(REPLACE "|" ";" fields "${pair}")
  list(GET fields 0 name)
  list(GET fields 1 value)
  require_text(resource_text "VALUE \"${name}\", \"${value}\\0\""
    "${name} string resource")
endforeach()
require_text(cmake_text "enable_language(RC)" "Windows RC language enablement")
require_text(cmake_text "src/bennu-version.rc.in" "configured resource input")
require_text(cmake_text "bennu-version.rc" "configured resource target source")
require_text(package_text "Assert-WindowsVersionResource" "package-time PE inspection")
require_text(package_text ".VersionInfo" "PowerShell VersionInfo inspection")
require_text(package_text "extracted --version" "extracted Windows/macOS version journey")
