execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${FC_TEST_SOURCE}"
        -B "${FC_TEST_BINARY}"
        "-DFC_PROJECT_ROOT=${FC_PROJECT_ROOT}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)

set(configure_log "${configure_output}\n${configure_error}")
if(configure_result EQUAL 0)
    message(FATAL_ERROR "Duplicate patch configuration unexpectedly succeeded.\n${configure_log}")
endif()
if(NOT configure_log MATCHES "Duplicate patch ID 'Duplicate'")
    message(FATAL_ERROR "Duplicate patch configuration failed for the wrong reason.\n${configure_log}")
endif()
