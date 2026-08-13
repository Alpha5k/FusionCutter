if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

set(direct_transport_source "${FC_SOURCE_ROOT}/src/patches/networking/direct_transport")

add_executable(direct_transport_tests
    protocol_tests.cpp
    policy_tests.cpp
    transport_tests.cpp
    "${direct_transport_source}/shared/datagram.cpp"
    "${direct_transport_source}/shared/galaxy.cpp"
    "${direct_transport_source}/shared/native_packet.cpp"
    "${direct_transport_source}/shared/protocol.cpp"
    "${direct_transport_source}/shared/security.cpp"
    "${direct_transport_source}/shared/socket.cpp"
    "${direct_transport_source}/shared/thread_affinity.cpp"
)

target_include_directories(direct_transport_tests PRIVATE
    "${direct_transport_source}"
)

target_link_libraries(direct_transport_tests PRIVATE
    Catch2::Catch2WithMain
    Bcrypt
    fusioncutter_config
    Ws2_32
)

fc_configure_project_target(direct_transport_tests)
fc_set_artifact_directories(direct_transport_tests)

add_test(NAME patches.direct_transport COMMAND direct_transport_tests)
