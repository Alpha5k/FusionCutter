add_test(NAME fc_package_consumer
    COMMAND "${CMAKE_COMMAND}"
        "-DFC_BUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
        "-DFC_CONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/consumer"
        "-DFC_CONFIGURATION=$<CONFIG>"
        "-DFC_GENERATOR=${CMAKE_GENERATOR}"
        "-DFC_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DFC_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_package_consumer.cmake"
)
set_tests_properties(fc_package_consumer PROPERTIES LABELS integration)
