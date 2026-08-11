fc_patch(
    ID DoorCorpseFix
    DEFINITION fusioncutter::patches::door_corpse_fix::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        patch.cpp
        door_corpse_fix.cpp
)
