fc_patch(
    ID SpawnJumpFix
    DEFINITION fusioncutter::patches::spawn_jump_fix::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        patch.cpp
        spawn_jump_fix.cpp
)
