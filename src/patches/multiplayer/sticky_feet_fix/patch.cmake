fc_patch(
    ID StickyFeetFix
    DEFINITION fusioncutter::patches::sticky_feet_fix::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        patch.cpp
        sticky_feet_fix.cpp
)
