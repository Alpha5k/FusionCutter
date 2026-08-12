fc_patch(
    ID LateUpdateRecovery
    DEFINITION fusioncutter::patches::update_recovery::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        layout.cpp
        patch.cpp
        update_recovery.cpp
)
