if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

add_executable(rcon_tests
    abi_tests.cpp
    gog_tests.cpp
    "${FC_SOURCE_ROOT}/src/patches/server/rcon_server/gog/command.cpp"
    "${FC_SOURCE_ROOT}/src/patches/server/rcon_server/gog/protocol.cpp"
)

target_include_directories(rcon_tests PRIVATE
    "${FC_SOURCE_ROOT}/src/patches/server/rcon_server"
)

target_link_libraries(rcon_tests PRIVATE
    Catch2::Catch2WithMain
)

fc_configure_project_target(rcon_tests)
fc_set_artifact_directories(rcon_tests)

add_test(NAME patches.rcon COMMAND rcon_tests)
