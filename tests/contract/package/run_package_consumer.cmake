if(NOT DEFINED FC_BUILD_DIRECTORY OR NOT DEFINED FC_CONSUMER_SOURCE OR NOT DEFINED FC_CONFIGURATION)
    message(FATAL_ERROR "The package consumer test is missing its required paths or configuration.")
endif()

set(test_root "${FC_BUILD_DIRECTORY}/package-consumer/${FC_CONFIGURATION}")
set(prefix "${test_root}/prefix")
set(consumer_build "${test_root}/build")
# Recreate only this configuration's isolated test root so no in-tree targets can satisfy the consumer accidentally.
file(REMOVE_RECURSE "${test_root}")

# Install the SDK into a private prefix exactly as a downstream consumer would receive it.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${FC_BUILD_DIRECTORY}" --config "${FC_CONFIGURATION}" --prefix "${prefix}"
    RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "Installing FusionCutterSDK failed with exit code ${install_result}.")
endif()

# Configure a clean project against only the private prefix while preserving the active generator platform/toolset.
set(configure_command
    "${CMAKE_COMMAND}"
    -S "${FC_CONSUMER_SOURCE}"
    -B "${consumer_build}"
    -G "${FC_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${prefix}"
)
if(NOT FC_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_command -A "${FC_GENERATOR_PLATFORM}")
endif()
if(NOT FC_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_command -T "${FC_GENERATOR_TOOLSET}")
endif()

execute_process(COMMAND ${configure_command} RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "Configuring the clean SDK consumer failed with exit code ${configure_result}.")
endif()

# A successful downstream build proves the exported target, headers, and generated plugin bridge are self-contained.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config "${FC_CONFIGURATION}"
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Building the clean SDK consumer failed with exit code ${build_result}.")
endif()
