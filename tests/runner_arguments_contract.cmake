foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

# TEST-ID: PARG-006-TEXT-GRAMMAR
# TEST-ID: PARG-007-RUNNER-SEPARATOR
# TEST-ID: PARG-014-DIAGNOSTICS
# TEST-ID: PARG-015-ATOMIC-STDOUT
# TEST-ID: PARG-018-PLATFORMS
function(check_runner name source expected_exit expected_stdout expected_stderr)
  set(source_file "${CMAKE_CURRENT_BINARY_DIR}/bennu-runner-${name}.bennu")
  file(WRITE "${source_file}" "${source}")
  execute_process(
    COMMAND "${BENNU_EXECUTABLE}" run "${source_file}" ${ARGN}
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
  )
  file(REMOVE "${source_file}")
  string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
  string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")
  string(REPLACE "<SOURCE>" "${source_file}" expected_stderr "${expected_stderr}")
  if(NOT "${actual_exit}" STREQUAL "${expected_exit}" OR
     NOT actual_stdout STREQUAL expected_stdout OR
     NOT actual_stderr STREQUAL expected_stderr)
    message(FATAL_ERROR
      "${name}: runner argument contract mismatch\n"
      "command arguments: [${ARGN}]\n"
      "exit expected/actual: ${expected_exit}/${actual_exit}\n"
      "stdout expected: [${expected_stdout}]\n"
      "stdout actual:   [${actual_stdout}]\n"
      "stderr expected: [${expected_stderr}]\n"
      "stderr actual:   [${actual_stderr}]"
    )
  endif()
endfunction()

set(int_source "parameters[value Int]\nvalue\n")
set(int_span "12:1:12-21:1:21")
set(double_source "parameters[value Double]\nvalue\n")
set(double_span "12:1:12-24:1:24")
set(bool_source "parameters[value Bool]\nvalue\n")
set(bool_span "12:1:12-22:1:22")

check_runner(typed_scalar_triplet
  "parameters[n Int delta Double enabled Bool]\nn\ndelta\nenabled\n"
  0 "5\n2.5\ntrue\n" "" -- 5 2.5 true)
check_runner(negative_values_are_data
  "parameters[n Int z Double]\nn\nz\n"
  0 "-5\n-0.0\n" "" -- -5 -0.0)

foreach(value IN ITEMS true false)
  check_runner("bool_${value}" "${bool_source}" 0 "${value}\n" "" -- "${value}")
endforeach()
foreach(value IN ITEMS True FALSE 1 truex)
  check_runner("bool_invalid_${value}" "${bool_source}" 1 ""
    "bennu_argument_error reason=invalid_literal required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Bool declaration_span=${bool_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
    -- "${value}")
endforeach()

foreach(value IN ITEMS 0 -1 9223372036854775807 -9223372036854775808)
  check_runner("int_valid_${value}" "${int_source}" 0 "${value}\n" "" -- "${value}")
endforeach()
foreach(value IN ITEMS +1 -0 00 01 1x)
  check_runner("int_invalid_${value}" "${int_source}" 1 ""
    "bennu_argument_error reason=invalid_literal required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Int declaration_span=${int_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
    -- "${value}")
endforeach()
foreach(value IN ITEMS 9223372036854775808 -9223372036854775809)
  check_runner("int_range_${value}" "${int_source}" 1 ""
    "bennu_argument_error reason=out_of_range required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Int declaration_span=${int_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
    -- "${value}")
endforeach()

set(double_valid_values
  0.0 0e0 -0.0 -0e0 0.5 2.0 2e0 1.25E-2 inf -inf nan
  4.9406564584124654e-324 1.7976931348623157e308 1e-9999 -1e-9999)
set(double_valid_outputs
  0.0 0.0 -0.0 -0.0 0.5 2.0 2.0 0.0125 inf -inf nan
  5e-324 1.7976931348623157e308 0.0 -0.0)
list(LENGTH double_valid_values double_valid_count)
math(EXPR double_valid_last "${double_valid_count} - 1")
foreach(index RANGE ${double_valid_last})
  list(GET double_valid_values ${index} value)
  list(GET double_valid_outputs ${index} output)
  check_runner("double_valid_${index}" "${double_source}" 0 "${output}\n" "" -- "${value}")
endforeach()
foreach(value IN ITEMS 2 +2.0 01.0 .5 1. 1e 1.0x +inf -nan)
  check_runner("double_invalid_${value}" "${double_source}" 1 ""
    "bennu_argument_error reason=invalid_literal required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Double declaration_span=${double_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
    -- "${value}")
endforeach()
foreach(value IN ITEMS 1e9999 -1e9999)
  check_runner("double_range_${value}" "${double_source}" 1 ""
    "bennu_argument_error reason=out_of_range required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Double declaration_span=${double_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
    -- "${value}")
endforeach()

check_runner(missing_count "${int_source}" 1 ""
  "bennu_argument_error reason=missing required_count=1 supplied_count=0 position=1 parameter_name=value expected_type=Int declaration_span=${int_span} actual_container=- actual_type=- invalid_value_invariant=-\n")
check_runner(explicit_missing_count "${int_source}" 1 ""
  "bennu_argument_error reason=missing required_count=1 supplied_count=0 position=1 parameter_name=value expected_type=Int declaration_span=${int_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
  --)
check_runner(extra_precedes_decode "${int_source}" 1 ""
  "bennu_argument_error reason=extra required_count=1 supplied_count=2 position=2 parameter_name=- expected_type=- declaration_span=- actual_container=- actual_type=- invalid_value_invariant=-\n"
  -- malformed 2)
check_runner(lowest_invalid_position
  "parameters[first Int second Bool]\nfirst\n"
  1 ""
  "bennu_argument_error reason=invalid_literal required_count=2 supplied_count=2 position=1 parameter_name=first expected_type=Int declaration_span=12:1:12-21:1:21 actual_container=- actual_type=- invalid_value_invariant=-\n"
  -- bad nope)
check_runner(static_error_precedes_decode
  "parameters[value Int]\nvalue\nwat[1]\n"
  1 ""
  "<SOURCE>:3:1: UnknownPrimitive: unknown primitive 'wat'\n"
  -- malformed)

check_runner(zero_parameter_implicit "1\n" 0 "1\n" "")
check_runner(zero_parameter_explicit "1\n" 0 "1\n" "" --)
check_runner(zero_parameter_extra "1\n" 1 ""
  "bennu_argument_error reason=extra required_count=0 supplied_count=1 position=1 parameter_name=- expected_type=- declaration_span=- actual_container=- actual_type=- invalid_value_invariant=-\n"
  -- extra)
check_runner(parameterized_zero_roots
  "parameters[value Int]\n" 0 "" "" -- 5)
check_runner(parameterized_zero_roots_invalid
  "parameters[value Int]\n" 1 ""
  "bennu_argument_error reason=invalid_literal required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Int declaration_span=${int_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
  -- invalid)
check_runner(second_separator_is_data "${int_source}" 1 ""
  "bennu_argument_error reason=invalid_literal required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Int declaration_span=${int_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
  -- --)

check_runner(evaluation_failure_is_atomic
  "parameters[value Int]\ninc[value]\ninc[9223372036854775807]\n"
  1 ""
  "<SOURCE>:3:1: DomainError: inc failed: integer_overflow\n"
  -- 5)

string(ASCII 27 escape)
set(hostile "${escape}[31m%s%s$(touch_should_not_exist)")
check_runner(hostile_argument_is_omitted "${int_source}" 1 ""
  "bennu_argument_error reason=invalid_literal required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Int declaration_span=${int_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
  -- "${hostile}")
check_runner(non_ascii_argument_is_omitted "${bool_source}" 1 ""
  "bennu_argument_error reason=invalid_literal required_count=1 supplied_count=1 position=1 parameter_name=value expected_type=Bool declaration_span=${bool_span} actual_container=- actual_type=- invalid_value_invariant=-\n"
  -- "é")

set(locale_source "parameters[value Double]\nvalue\n")
set(locale_file "${CMAKE_CURRENT_BINARY_DIR}/bennu-runner-locale.bennu")
file(WRITE "${locale_file}" "${locale_source}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LC_ALL=de_DE.UTF-8"
          "${BENNU_EXECUTABLE}" run "${locale_file}" -- 1.5
  RESULT_VARIABLE locale_exit OUTPUT_VARIABLE locale_stdout ERROR_VARIABLE locale_stderr)
file(REMOVE "${locale_file}")
string(REPLACE "\r\n" "\n" locale_stdout "${locale_stdout}")
string(REPLACE "\r\n" "\n" locale_stderr "${locale_stderr}")
if(NOT "${locale_exit}" STREQUAL "0" OR
   NOT locale_stdout STREQUAL "1.5\n" OR NOT locale_stderr STREQUAL "")
  message(FATAL_ERROR
    "locale-independent Double decode failed\nexit: ${locale_exit}\n"
    "stdout: [${locale_stdout}]\nstderr: [${locale_stderr}]")
endif()

set(missing_file "${CMAKE_CURRENT_BINARY_DIR}/bennu-runner-missing.bennu")
file(REMOVE "${missing_file}")
execute_process(
  COMMAND "${BENNU_EXECUTABLE}" run "${missing_file}" stray
  RESULT_VARIABLE usage_exit OUTPUT_VARIABLE usage_stdout ERROR_VARIABLE usage_stderr)
string(REPLACE "\r\n" "\n" usage_stderr "${usage_stderr}")
if("${usage_exit}" STREQUAL "0" OR NOT usage_stdout STREQUAL "" OR
   NOT usage_stderr STREQUAL
       "error: expected 'run <source> [-- <arguments...>]'\n")
  message(FATAL_ERROR
    "runner usage must win before source I/O\nexit: ${usage_exit}\n"
    "stdout: [${usage_stdout}]\nstderr: [${usage_stderr}]")
endif()
