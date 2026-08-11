fc_patch(
    ID DLCMissionLimit
    DEFINITION fusioncutter::patches::dlc_mission_limit::definition
    ARCHITECTURES X86
    ROLES CLIENT SERVER
    SOURCES
        patch.cpp
        dlc_mission_limit.cpp
)
