add_library(fc_shared_hook_observer_probe SHARED
    observer_probe.cpp
    ../shared_hook_probe.hpp
)

fc_configure_project_target(fc_shared_hook_observer_probe)
fc_set_artifact_directories(fc_shared_hook_observer_probe)

target_include_directories(fc_shared_hook_observer_probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(fc_shared_hook_observer_probe PRIVATE
    FusionCutter::SDK
)
