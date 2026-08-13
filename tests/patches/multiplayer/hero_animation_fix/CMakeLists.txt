if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

set(hero_animation_source "${FC_SOURCE_ROOT}/src/patches/multiplayer/hero_animation_fix")

add_executable(hero_animation_fix_tests
    history_tests.cpp
    "${hero_animation_source}/history.cpp"
)

target_include_directories(hero_animation_fix_tests PRIVATE
    "${hero_animation_source}"
)

target_link_libraries(hero_animation_fix_tests PRIVATE
    Catch2::Catch2WithMain
)

fc_configure_project_target(hero_animation_fix_tests)
fc_set_artifact_directories(hero_animation_fix_tests)

add_test(NAME patches.hero_animation_fix COMMAND hero_animation_fix_tests)
