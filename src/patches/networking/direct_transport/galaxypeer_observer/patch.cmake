fc_patch(
    ID GalaxyPeerObserver
    DEFINITION fusioncutter::patches::galaxypeer_observer::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        observer.cpp
        patch.cpp
)
