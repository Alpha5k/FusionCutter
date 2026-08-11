fc_patch(
    ID CrouchBugFix
    DEFINITION fusioncutter::patches::crouch_bug_fix::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        patch.cpp
        crouch_bug_fix.cpp
)
