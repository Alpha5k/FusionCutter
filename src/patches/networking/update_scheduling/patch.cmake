fc_patch(
    ID UpdateScheduling
    DEFINITION fusioncutter::patches::update_scheduling::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        patch.cpp
        update_scheduling.cpp
)
