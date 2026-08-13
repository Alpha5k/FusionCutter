fc_patch(
    ID UpdatePacing
    DEFINITION fusioncutter::patches::update_pacing::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        patch.cpp
        update_pacing.cpp
)
