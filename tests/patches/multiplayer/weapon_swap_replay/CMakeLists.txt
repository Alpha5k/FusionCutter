if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

set(weapon_swap_source "${FC_SOURCE_ROOT}/src/patches/multiplayer/weapon_swap_replay")

add_executable(weapon_swap_replay_tests
    reconciliation_tests.cpp
    "${weapon_swap_source}/local_ledger.cpp"
    "${weapon_swap_source}/packed_ledger.cpp"
)

target_include_directories(weapon_swap_replay_tests PRIVATE
    "${weapon_swap_source}"
)

target_link_libraries(weapon_swap_replay_tests PRIVATE
    Catch2::Catch2WithMain
)

fc_configure_project_target(weapon_swap_replay_tests)
fc_set_artifact_directories(weapon_swap_replay_tests)

add_test(NAME patches.weapon_swap_replay COMMAND weapon_swap_replay_tests)
