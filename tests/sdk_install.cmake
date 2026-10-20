# Install and relocate the SDK, then build its example as an independent consumer.
cmake_path(ABSOLUTE_PATH FC_BUILD_DIRECTORY NORMALIZE OUTPUT_VARIABLE build_root)
set(test_root "${build_root}/sdk-install/${FC_CONFIGURATION}")
cmake_path(IS_PREFIX build_root "${test_root}" NORMALIZE confined)
if(NOT confined)
    message(FATAL_ERROR "SDK test output must remain inside the build directory.")
endif()
file(REMOVE_RECURSE "${test_root}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${FC_BUILD_DIRECTORY}" --config "${FC_CONFIGURATION}"
        --prefix "${test_root}/sdk" --component SDK
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installing the SDK failed (process result: ${result}).")
endif()

file(RENAME "${test_root}/sdk" "${test_root}/relocated")
set(prefix "${test_root}/relocated")
set(consumer_build "${test_root}/build")
set(configure_command
    "${CMAKE_COMMAND}" -S "${prefix}/example" -B "${consumer_build}" -G "${FC_GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${FC_CONFIGURATION}" "-DCMAKE_PREFIX_PATH=${prefix}"
)
if(FC_MAKE_PROGRAM)
    list(APPEND configure_command "-DCMAKE_MAKE_PROGRAM=${FC_MAKE_PROGRAM}")
endif()
execute_process(COMMAND ${configure_command} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Configuring the SDK example failed (process result: ${result}).")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config "${FC_CONFIGURATION}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Building the SDK example failed (process result: ${result}).")
endif()

# Load the separately built example through the runtime's plugin loader.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "FC_EXTERNAL_PLUGIN=${consumer_build}/ExamplePlugin.dll"
        "${FC_TEST_EXECUTABLE}" "[.external]"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Loading the SDK example failed (process result: ${result}).")
endif()
