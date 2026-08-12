fc_patch(
    ID InactiveWindowFix
    DEFINITION fusioncutter::patches::inactive_window_fix::definition
    ARCHITECTURES X86
    ROLES CLIENT SERVER
    SOURCES
        patch.cpp
)
