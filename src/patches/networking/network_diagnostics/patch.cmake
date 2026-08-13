fc_patch(
    ID NetworkDiagnostics
    DEFINITION fusioncutter::patches::network_diagnostics::definition
    ARCHITECTURES X86
    ROLES CLIENT SERVER
    SOURCES
        combat.cpp
        network_diagnostics.cpp
        observers.cpp
        patch.cpp
        trace/recorder.cpp
)

fc_patch_sources(
    PATCH NetworkDiagnostics
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        client/observers.cpp
        client/codec.cpp
        client/combat.cpp
)

fc_patch_sources(
    PATCH NetworkDiagnostics
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        server/observers.cpp
        server/codec.cpp
        server/combat.cpp
)
