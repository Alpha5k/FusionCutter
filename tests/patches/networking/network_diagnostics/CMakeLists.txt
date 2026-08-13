if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

add_executable(network_diagnostics_tests
    recorder_tests.cpp
)

target_link_libraries(network_diagnostics_tests PRIVATE
    Catch2::Catch2WithMain
)

fc_configure_project_target(network_diagnostics_tests)
fc_set_artifact_directories(network_diagnostics_tests)

add_test(NAME patches.network_diagnostics COMMAND network_diagnostics_tests)
