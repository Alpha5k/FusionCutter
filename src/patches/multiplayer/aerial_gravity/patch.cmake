fc_patch(
    ID AerialGravity
    DEFINITION fusioncutter::patches::aerial_gravity::definition
    ARCHITECTURES X86
    ROLES CLIENT SERVER
    SOURCES
        aerial_gravity.cpp
        patch.cpp
)
