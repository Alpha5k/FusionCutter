if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

add_executable(update_pacing_tests
    policy_tests.cpp
)

target_include_directories(update_pacing_tests PRIVATE
    "${FC_SOURCE_ROOT}/src/patches/networking/update_pacing"
)

target_link_libraries(update_pacing_tests PRIVATE
    Catch2::Catch2WithMain
)

fc_configure_project_target(update_pacing_tests)
fc_set_artifact_directories(update_pacing_tests)

add_test(NAME patches.update_pacing COMMAND update_pacing_tests)
