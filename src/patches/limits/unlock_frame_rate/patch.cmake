fc_patch(
    ID UnlockFrameRate
    DEFINITION fusioncutter::patches::unlock_frame_rate::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        patch.cpp
        unlock_frame_rate.cpp
)
