if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

set(update_recovery_source "${FC_SOURCE_ROOT}/src/patches/networking/update_recovery")

add_executable(update_recovery_tests
    ordering_tests.cpp
)

target_include_directories(update_recovery_tests PRIVATE
    "${update_recovery_source}"
)

target_link_libraries(update_recovery_tests PRIVATE
    Catch2::Catch2WithMain
)

fc_configure_project_target(update_recovery_tests)
fc_set_artifact_directories(update_recovery_tests)

add_test(NAME patches.update_recovery COMMAND update_recovery_tests)
