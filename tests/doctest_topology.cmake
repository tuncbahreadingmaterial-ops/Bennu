if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

function(require_file path description)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "missing ${description}: ${path}")
  endif()
endfunction()

function(require_text text expected description)
  string(FIND "${text}" "${expected}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "missing ${description}: ${expected}")
  endif()
endfunction()

set(doctest_root "${BENNU_SOURCE_DIR}/third_party/doctest")
set(doctest_header "${doctest_root}/doctest/doctest.h")
set(doctest_license "${doctest_root}/LICENSE.txt")
set(doctest_record "${doctest_root}/README.bennu.md")
require_file("${doctest_header}" "vendored doctest header")
require_file("${doctest_license}" "vendored doctest license")
require_file("${doctest_record}" "doctest provenance record")

file(SHA256 "${doctest_header}" doctest_header_sha256)
if(NOT doctest_header_sha256 STREQUAL
   "cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421")
  message(FATAL_ERROR
    "vendored doctest header differs from the pinned upstream revision")
endif()
file(SHA256 "${doctest_license}" doctest_license_sha256)
if(NOT doctest_license_sha256 STREQUAL
   "0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5")
  message(FATAL_ERROR
    "vendored doctest license differs from the pinned upstream revision")
endif()

file(READ "${doctest_record}" doctest_record_text)
require_text("${doctest_record_text}" "v2.5.3" "exact doctest version")
require_text("${doctest_record_text}"
  "2d0a9359a60c51affe2a9bebb1be1dca47868151"
  "exact doctest revision")
require_text("${doctest_record_text}" "MIT" "doctest license accounting")

set(cmake_path "${BENNU_SOURCE_DIR}/CMakeLists.txt")
file(READ "${cmake_path}" cmake_text)
string(FIND "${cmake_text}" "add_library(bennu_core" core_target_at)
if(NOT core_target_at EQUAL -1)
  message(FATAL_ERROR "bennu_core target remains in CMake configuration")
endif()
foreach(required_cmake_text IN ITEMS
    "set(bennu_implementation_sources"
    "add_executable(bennu"
    "add_executable(bennu_tests"
    "DOCTEST_CONFIG_DISABLE"
    "DOCTEST_CONFIG_NO_EXCEPTIONS"
    "DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS"
    "tests/doctest_main.cpp"
    "architecture.doctest_topology")
  require_text("${cmake_text}" "${required_cmake_text}"
    "doctest build topology")
endforeach()
foreach(removed_test_source IN ITEMS
    "tests/evaluator_test.cpp"
    "tests/c_emitter_test.cpp"
    "tests/native_builder_test.cpp")
  string(FIND "${cmake_text}" "${removed_test_source}" old_test_at)
  if(NOT old_test_at EQUAL -1)
    message(FATAL_ERROR
      "standalone unit-test source remains in CMake: ${removed_test_source}")
  endif()
  if(EXISTS "${BENNU_SOURCE_DIR}/${removed_test_source}")
    message(FATAL_ERROR
      "standalone unit-test source was not removed: ${removed_test_source}")
  endif()
endforeach()

set(doctest_main "${BENNU_SOURCE_DIR}/tests/doctest_main.cpp")
require_file("${doctest_main}" "doctest test main")
file(READ "${doctest_main}" doctest_main_text)
require_text("${doctest_main_text}" "DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN"
  "the single doctest implementation and main")

set(doctest_implementation_count 0)
file(GLOB_RECURSE bennu_cpp_sources
  "${BENNU_SOURCE_DIR}/src/*.cpp"
  "${BENNU_SOURCE_DIR}/tests/*.cpp")
foreach(source IN LISTS bennu_cpp_sources)
  file(READ "${source}" source_text)
  string(REGEX MATCHALL "DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN"
    implementation_markers "${source_text}")
  list(LENGTH implementation_markers source_implementation_count)
  math(EXPR doctest_implementation_count
    "${doctest_implementation_count} + ${source_implementation_count}")
endforeach()
if(NOT doctest_implementation_count EQUAL 1)
  message(FATAL_ERROR
    "expected exactly one doctest implementation/main marker, found ${doctest_implementation_count}")
endif()

set(evaluator_header "${BENNU_SOURCE_DIR}/include/bennu/evaluator.hpp")
file(READ "${evaluator_header}" evaluator_header_text)
foreach(internal_name IN ITEMS
    "TokenKind"
    "TokenizeResult"
    "ParseResult"
    "ExpressionKind"
    "ExpressionNode"
    "struct Program {"
    "parse_program"
    "evaluate_program"
    "tokenize("
    "apply_inc"
    "invalid_program")
  string(FIND "${evaluator_header_text}" "${internal_name}" exposed_at)
  if(NOT exposed_at EQUAL -1)
    message(FATAL_ERROR
      "evaluator internal remains in the public header: ${internal_name}")
  endif()
endforeach()

foreach(implementation IN ITEMS application native_builder rewrite)
  set(source "${BENNU_SOURCE_DIR}/src/${implementation}.cpp")
  file(READ "${source}" source_text)
  require_text("${source_text}" "doctest/doctest.h"
    "embedded doctest include in ${implementation}.cpp")
  require_text("${source_text}" "TEST_CASE("
    "embedded doctest cases in ${implementation}.cpp")
endforeach()
