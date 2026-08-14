fc_patch(
    ID HeroDiagnostics
    DEFINITION fusioncutter::patches::hero_diagnostics::definition
    ARCHITECTURES X86
    ROLES CLIENT SERVER
    SOURCES
        hero_diagnostics.cpp
        layout.cpp
        patch.cpp
        subject.cpp
)

fc_patch_sources(
    PATCH HeroDiagnostics
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        client/observers.cpp
)

fc_patch_sources(
    PATCH HeroDiagnostics
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        server/observers.cpp
)
