fc_patch(
    ID SpectatorCameraSmoothing
    DEFINITION fusioncutter::patches::spectator_camera::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        layout.cpp
        patch.cpp
        smoothing.cpp
        spectator_camera.cpp
)
