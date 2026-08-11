fc_patch(
    ID Alpha
    DEFINITION alpha::definition
    ARCHITECTURES X86 X64
    ROLES CLIENT SERVER
    SOURCES
        definition.cpp
)

fc_patch_sources(
    PATCH Alpha
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        x86_client.cpp
)
