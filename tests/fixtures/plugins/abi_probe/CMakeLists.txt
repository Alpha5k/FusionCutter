# The fixture with a direct export is restaged under scenario names to exercise the full admission matrix.
add_library(fc_abi_admission_probe MODULE
    abi_probe.cpp
)

fc_configure_project_target(fc_abi_admission_probe)
fc_set_artifact_directories(fc_abi_admission_probe)

target_link_libraries(fc_abi_admission_probe PRIVATE
    FusionCutter::SDK
)

set_target_properties(fc_abi_admission_probe PROPERTIES
    OUTPUT_NAME "fc_abi_admission_probe"
)

# The module-definition file forwards the query export so discovery can reject it without execution.
add_library(fc_forwarded_query_probe MODULE
    forwarded_query.cpp
    forwarded_query.def
)

fc_configure_project_target(fc_forwarded_query_probe)
fc_set_artifact_directories(fc_forwarded_query_probe)

set_target_properties(fc_forwarded_query_probe PROPERTIES
    OUTPUT_NAME "fc_forwarded_query_probe"
)
