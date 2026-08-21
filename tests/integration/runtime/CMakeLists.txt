fc_add_catch_test(fc_shared_hook_prototype_tests
    NAME fc_shared_hook_prototype
    LABEL integration
    SOURCES
        shared_hook_prototype_tests.cpp
        ../../fixtures/plugins/shared_hook_probe.hpp
    LIBRARIES
        FusionCutter::SDK
        fusioncutter_catalog
        fusioncutter_runtime
)

target_include_directories(fc_shared_hook_prototype_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/plugins"
)

target_compile_definitions(fc_shared_hook_prototype_tests PRIVATE
    FC_SHARED_HOOK_OWNER_PATH="$<TARGET_FILE:fc_shared_hook_owner_probe>"
    FC_SHARED_HOOK_OBSERVER_PATH="$<TARGET_FILE:fc_shared_hook_observer_probe>"
)

add_dependencies(fc_shared_hook_prototype_tests
    fc_shared_hook_observer_probe
    fc_shared_hook_owner_probe
)

fc_add_catch_test(fc_interface_binary_tests
    NAME fc_interface_binary
    LABEL integration
    SOURCES interface_binary_tests.cpp
    LIBRARIES
        fusioncutter_catalog
        fusioncutter_runtime
)
target_compile_definitions(fc_interface_binary_tests PRIVATE
    FC_INTERFACE_PROVIDER_PATH=L"$<TARGET_FILE:fc_interface_provider_probe>"
)
add_dependencies(fc_interface_binary_tests fc_interface_provider_probe)
