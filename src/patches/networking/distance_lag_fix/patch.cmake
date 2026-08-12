fc_patch(
    ID DistanceLagFix
    DEFINITION fusioncutter::patches::distance_lag_fix::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        patch.cpp
)
