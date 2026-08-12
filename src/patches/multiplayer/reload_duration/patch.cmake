fc_patch(
    ID ReloadDurationFix
    DEFINITION fusioncutter::patches::reload_duration::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        patch.cpp
        reload_duration.cpp
)
