fc_patch(
    ID SoldierStatePipeline
    DEFINITION fusioncutter::patches::soldier_state_pipeline::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        patch.cpp
        pipeline.cpp
)
