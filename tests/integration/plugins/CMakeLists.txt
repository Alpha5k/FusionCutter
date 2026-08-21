fc_add_catch_test(fc_plugin_admission_tests
    NAME fc_plugin_admission
    LABEL integration
    SOURCES admission_tests.cpp
    LIBRARIES fusioncutter_catalog
)

# Pass resolved paths so tests stage the selected configuration's binaries under scenario names.
target_compile_definitions(fc_plugin_admission_tests PRIVATE
    FC_ABI_PROBE_PATH="$<TARGET_FILE:fc_abi_admission_probe>"
    FC_FORWARDER_PROBE_PATH="$<TARGET_FILE:fc_forwarded_query_probe>"
    FC_DEPENDENT_PROBE_PATH="$<TARGET_FILE:fc_dependent_dll_probe>"
    FC_DEPENDENCY_PATH="$<TARGET_FILE:fc_probe_dependency>"
    FC_SDK_PROBE_PATH="$<TARGET_FILE:fc_sdk_external_contract>"
)

add_dependencies(fc_plugin_admission_tests
    fc_abi_admission_probe
    fc_forwarded_query_probe
    fc_dependent_dll_probe
    fc_probe_dependency
    fc_sdk_external_contract
)
