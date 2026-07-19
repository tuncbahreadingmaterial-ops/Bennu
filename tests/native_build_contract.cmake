foreach(required BENNU_EXECUTABLE BENNU_SOURCE_DIR BENNU_C_COMPILER
                 BENNU_EXECUTABLE_SUFFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/native build contract")
set(source_file "${work_directory}/detached source.bennu")
file(REMOVE_RECURSE "${work_directory}")
file(MAKE_DIRECTORY "${work_directory}")
configure_file("${BENNU_SOURCE_DIR}/examples/level1.bennu" "${source_file}" COPYONLY)

foreach(selection explicit environment fallback)
  set(output_file
      "${work_directory}/detached level1 ${selection}${BENNU_EXECUTABLE_SUFFIX}")
  if(selection STREQUAL "explicit")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env --unset=CC
              "${BENNU_EXECUTABLE}" build "${source_file}" -o "${output_file}"
              --cc "${BENNU_C_COMPILER}"
      RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
      ERROR_VARIABLE build_stderr)
  elseif(selection STREQUAL "environment")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "CC=${BENNU_C_COMPILER}"
              "${BENNU_EXECUTABLE}" build "${source_file}" -o "${output_file}"
      RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
      ERROR_VARIABLE build_stderr)
  else()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env --unset=CC
              "${BENNU_EXECUTABLE}" build "${source_file}" -o "${output_file}"
      RESULT_VARIABLE build_exit OUTPUT_VARIABLE build_stdout
      ERROR_VARIABLE build_stderr)
  endif()
  if(NOT "${build_exit}" STREQUAL "0" OR NOT build_stdout STREQUAL "" OR
     NOT build_stderr STREQUAL "" OR NOT EXISTS "${output_file}")
    message(FATAL_ERROR
      "${selection} real-compiler native build failed\nexit: ${build_exit}\n"
      "stdout: [${build_stdout}]\nstderr: [${build_stderr}]")
  endif()
endforeach()

# The native artifacts must not need Bennu, generated C, or their source input.
file(REMOVE "${source_file}")
foreach(selection explicit environment fallback)
  set(output_file
      "${work_directory}/detached level1 ${selection}${BENNU_EXECUTABLE_SUFFIX}")
  execute_process(
    COMMAND "${output_file}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE native_exit
    OUTPUT_VARIABLE native_stdout
    ERROR_VARIABLE native_stderr
  )
  string(REPLACE "\r\n" "\n" native_stdout "${native_stdout}")
  if(NOT "${native_exit}" STREQUAL "0" OR
     NOT native_stdout STREQUAL ">>(1 2 3 4 5)\n>>6\n" OR
     NOT native_stderr STREQUAL "")
    message(FATAL_ERROR
      "${selection} detached native executable contract mismatch\n"
      "exit: ${native_exit}\nstdout: [${native_stdout}]\n"
      "stderr: [${native_stderr}]")
  endif()
endforeach()
file(REMOVE_RECURSE "${work_directory}")
