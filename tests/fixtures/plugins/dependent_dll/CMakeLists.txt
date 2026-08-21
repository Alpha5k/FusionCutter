# Build a private imported dependency beside the generated plugin to verify restricted DLL search behavior.
add_library(fc_probe_dependency SHARED
    dependency.cpp
)

fc_configure_project_target(fc_probe_dependency)
fc_set_artifact_directories(fc_probe_dependency)

set_target_properties(fc_probe_dependency PROPERTIES
    OUTPUT_NAME "fc_probe_dependency"
)

fc_add_plugin(fc_dependent_dll_probe
    FACTORY dependent_probe::build_plugin
    SOURCES dependent_plugin.cpp
)

target_link_libraries(fc_dependent_dll_probe PRIVATE
    fc_probe_dependency
)

set_target_properties(fc_dependent_dll_probe PROPERTIES
    OUTPUT_NAME "fc_dependent_dll_probe"
)
