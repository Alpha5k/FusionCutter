if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

set(spectator_camera_source "${FC_SOURCE_ROOT}/src/patches/multiplayer/spectator_camera")

add_executable(spectator_camera_tests
    smoothing_tests.cpp
    "${spectator_camera_source}/smoothing.cpp"
)

target_include_directories(spectator_camera_tests PRIVATE
    "${spectator_camera_source}"
)

target_link_libraries(spectator_camera_tests PRIVATE
    Catch2::Catch2WithMain
)

fc_configure_project_target(spectator_camera_tests)
fc_set_artifact_directories(spectator_camera_tests)

add_test(NAME patches.spectator_camera COMMAND spectator_camera_tests)
