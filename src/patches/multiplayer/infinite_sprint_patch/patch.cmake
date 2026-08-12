fc_patch(
    ID InfiniteSprintPatch
    DEFINITION fusioncutter::patches::infinite_sprint_patch::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        infinite_sprint.cpp
        patch.cpp
)
