if(NOT DEFINED MATHMAKE OR NOT DEFINED SOURCE OR
   NOT DEFINED EXPECTED OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "Missing native test arguments")
endif()

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")

message(STATUS "Building native test: ${SOURCE}")

execute_process(
    COMMAND "${MATHMAKE}" build "${SOURCE}" -o "${OUTPUT}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)

if(NOT "${build_result}" STREQUAL "0")
    message(FATAL_ERROR
        "Native compilation failed for ${SOURCE}\n"
        "Exit code: ${build_result}\n"
        "stdout:\n${build_stdout}\n"
        "stderr:\n${build_stderr}"
    )
endif()

if(NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR
        "Native compilation reported success but did not produce: ${OUTPUT}"
    )
endif()

message(STATUS "Running native test: ${OUTPUT}")

execute_process(
    COMMAND "${OUTPUT}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE execution_result
    OUTPUT_VARIABLE actual_output
    ERROR_VARIABLE execution_stderr
)

if(NOT "${execution_result}" STREQUAL "0")
    message(FATAL_ERROR
        "Native executable failed: ${OUTPUT}\n"
        "Exit code: ${execution_result}\n"
        "stderr:\n${execution_stderr}"
    )
endif()

file(READ "${EXPECTED}" expected_output)

# Normalize Windows and Unix line endings.
string(REPLACE "\r\n" "\n" actual_output "${actual_output}")
string(REPLACE "\r" "\n" actual_output "${actual_output}")

string(REPLACE "\r\n" "\n" expected_output "${expected_output}")
string(REPLACE "\r" "\n" expected_output "${expected_output}")

if(NOT "${actual_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR
        "Output mismatch in native test: ${SOURCE}\n"
        "Expected:\n${expected_output}\n"
        "Actual:\n${actual_output}\n"
        "Compiler stdout:\n${build_stdout}\n"
        "Compiler stderr:\n${build_stderr}\n"
        "Program stderr:\n${execution_stderr}"
    )
endif()

message(STATUS "Native test passed: ${SOURCE}")
