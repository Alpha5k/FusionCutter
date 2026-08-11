set(test_catalog_source "${CMAKE_CURRENT_SOURCE_DIR}/catalog/nested")
set(test_catalog_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/test_patch_catalog.cpp")
fc_generate_patch_catalog(
    PATCH_ROOT "${test_catalog_source}"
    OUTPUT "${test_catalog_generated}"
    OUT_SOURCES test_patch_sources
)

add_executable(catalog_tests
    catalog_tests.cpp
    "${test_catalog_generated}"
    ${test_patch_sources}
)

target_link_libraries(catalog_tests PRIVATE
    Catch2::Catch2WithMain
    fusioncutter_catalog
)

if(FC_ARCHITECTURE STREQUAL "x86" AND NOT FC_CORE_ROLE STREQUAL "Server")
    set(has_x86_client_source 1)
else()
    set(has_x86_client_source 0)
endif()

if(FC_CORE_ROLE STREQUAL "Server")
    set(test_catalog_client 0)
else()
    set(test_catalog_client 1)
endif()
if(FC_CORE_ROLE STREQUAL "Client")
    set(test_catalog_server 0)
else()
    set(test_catalog_server 1)
endif()

target_compile_definitions(catalog_tests PRIVATE
    FC_TEST_CATALOG_CLIENT=${test_catalog_client}
    FC_TEST_CATALOG_SERVER=${test_catalog_server}
    FC_TEST_HAS_X86_CLIENT_SOURCE=${has_x86_client_source}
)

fc_configure_project_target(catalog_tests)
fc_set_artifact_directories(catalog_tests)

add_test(NAME core.catalog COMMAND catalog_tests)

add_test(NAME core.catalog_duplicate
    COMMAND "${CMAKE_COMMAND}"
        "-DFC_TEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/catalog/duplicate"
        "-DFC_TEST_BINARY=${CMAKE_CURRENT_BINARY_DIR}/catalog_duplicate"
        "-DFC_PROJECT_ROOT=${FC_SOURCE_ROOT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/catalog/duplicate/check.cmake"
)

add_executable(config_tests
    config_tests.cpp
)

target_link_libraries(config_tests PRIVATE
    Catch2::Catch2WithMain
    fusioncutter_catalog
    fusioncutter_config
)

fc_configure_project_target(config_tests)
fc_set_artifact_directories(config_tests)

add_test(NAME core.config COMMAND config_tests)

add_executable(patching_tests
    patching_tests.cpp
)

target_link_libraries(patching_tests PRIVATE
    Catch2::Catch2WithMain
    fusioncutter_patching
)

fc_configure_project_target(patching_tests)
fc_set_artifact_directories(patching_tests)

add_test(NAME core.patching COMMAND patching_tests)

add_executable(startup_tests
    startup_tests.cpp
)

target_link_libraries(startup_tests PRIVATE
    Catch2::Catch2WithMain
    fusioncutter_startup
)

fc_configure_project_target(startup_tests)
fc_set_artifact_directories(startup_tests)

add_test(NAME core.startup COMMAND startup_tests)

add_executable(targets_tests
    targets_tests.cpp
)

target_link_libraries(targets_tests PRIVATE
    Catch2::Catch2WithMain
    fusioncutter_targets
)

fc_configure_project_target(targets_tests)
fc_set_artifact_directories(targets_tests)

add_test(NAME core.targets COMMAND targets_tests)

add_executable(reporting_probe
    reporting_probe.cpp
)

target_link_libraries(reporting_probe PRIVATE
    fusioncutter_reporting
)

fc_configure_project_target(reporting_probe)
fc_set_artifact_directories(reporting_probe)

add_executable(reporting_tests
    reporting_tests.cpp
)

target_link_libraries(reporting_tests PRIVATE
    Catch2::Catch2WithMain
    fusioncutter_reporting
)

fc_configure_project_target(reporting_tests)
fc_set_artifact_directories(reporting_tests)

add_dependencies(reporting_tests reporting_probe)

add_test(NAME core.reporting
    COMMAND "${CMAKE_COMMAND}" -E env
        "FC_REPORTING_PROBE=$<TARGET_FILE:reporting_probe>"
        "$<TARGET_FILE:reporting_tests>"
)

add_subdirectory(quill)
