fc_patch(
    ID DirectTransport
    DEFINITION fusioncutter::patches::direct_transport::definition
    ARCHITECTURES X86
    ROLES CLIENT SERVER
    SOURCES
        patch.cpp
        shared/datagram.cpp
        shared/diagnostics.cpp
        shared/galaxy.cpp
        shared/game_transport.cpp
        shared/game_layout.cpp
        shared/lobby_hooks.cpp
        shared/native_packet.cpp
        shared/protocol.cpp
        shared/security.cpp
        shared/socket.cpp
        shared/thread_affinity.cpp
)

fc_patch_sources(
    PATCH DirectTransport
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        client/associations.cpp
        client/direct_transport.cpp
        client/native.cpp
        client/transport.cpp
)

fc_patch_sources(
    PATCH DirectTransport
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        server/associations.cpp
        server/direct_transport.cpp
        server/endpoint.cpp
        server/native.cpp
        server/removals.cpp
        server/transport.cpp
)
