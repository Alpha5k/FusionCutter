fc_patch(
    ID MapHangFix
    DEFINITION fusioncutter::patches::map_hang_fix::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        map_hang_fix.cpp
        patch.cpp
)
