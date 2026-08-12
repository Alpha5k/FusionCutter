fc_patch(
    ID RconServer
    DEFINITION fusioncutter::patches::rcon_server::definition
    ARCHITECTURES X86 X64
    ROLES SERVER
    SOURCES
        patch.cpp
)

fc_patch_sources(
    PATCH RconServer
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        gog/authentication.cpp
        gog/client.cpp
        gog/command.cpp
        gog/game.cpp
        gog/protocol.cpp
        gog/rcon.cpp
        gog/service.cpp
)

fc_patch_sources(
    PATCH RconServer
    ARCHITECTURES X64
    ROLES SERVER
    SOURCES
        aspyr/rcon.cpp
)
