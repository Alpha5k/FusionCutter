fc_patch(
    ID NetworkPipeline
    DEFINITION fusioncutter::patches::network_pipeline::definition
    ARCHITECTURES X86
    ROLES CLIENT SERVER
    SOURCES
        patch.cpp
        pipeline.cpp
)
