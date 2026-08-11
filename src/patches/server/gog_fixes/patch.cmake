fc_patch(
    ID GOGServerFixes
    DEFINITION fusioncutter::patches::gog_fixes::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        patch.cpp
        gog_fixes.cpp
)
