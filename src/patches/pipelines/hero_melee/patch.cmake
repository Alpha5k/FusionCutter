fc_patch(
    ID HeroMeleePipeline
    DEFINITION fusioncutter::patches::hero_melee_pipeline::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        layout.cpp
        patch.cpp
        pipeline.cpp
)
