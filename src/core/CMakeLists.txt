# Admission, configuration, and target recognition share one object target so every entry path uses the same code.
add_library(fusioncutter_common_validation OBJECT
    core_logger.hpp
    fatal_boundary.cpp
    fatal_boundary.hpp
    catalog/catalog_builder.cpp
    catalog/catalog_builder.hpp
    catalog/callback_error.cpp
    catalog/callback_error.hpp
    catalog/catalog_types.cpp
    catalog/catalog_types.hpp
    catalog/configured_bundles.hpp
    catalog/definition_copy.cpp
    catalog/definition_copy.hpp
    catalog/native_library.cpp
    catalog/native_library.hpp
    catalog/pe_file.cpp
    catalog/pe_file.hpp
    catalog/plugin_discovery.cpp
    catalog/plugin_discovery.hpp
    config/configuration.cpp
    config/configuration.hpp
    config/configuration_types.hpp
    config/setting_value.cpp
    planning/native_call.cpp
    planning/native_call.hpp
    planning/native_address.cpp
    planning/native_address.hpp
    planning/evidence_validation.cpp
    planning/evidence_validation.hpp
    planning/plan_validation.cpp
    planning/plan_validation.hpp
    planning/planning_types.cpp
    planning/planning_types.hpp
    planning/resolution.cpp
    planning/resolution.hpp
    targets/image_view.cpp
    targets/image_view.hpp
    targets/mapped_file.cpp
    targets/mapped_file.hpp
    targets/recognition.cpp
    targets/recognition.hpp
    targets/target_profiles.cpp
    targets/target_profiles.hpp
)

# Stage 10 native fixtures reuse this exact source authority in a test profile variant rather than copying the list.
get_target_property(fc_common_validation_sources fusioncutter_common_validation SOURCES)
set(fc_common_validation_absolute_sources "")
foreach(fc_common_validation_source IN LISTS fc_common_validation_sources)
    if(IS_ABSOLUTE "${fc_common_validation_source}")
        list(APPEND fc_common_validation_absolute_sources "${fc_common_validation_source}")
    else()
        list(APPEND fc_common_validation_absolute_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/${fc_common_validation_source}"
        )
    endif()
endforeach()
set_property(GLOBAL PROPERTY FC_COMMON_VALIDATION_SOURCES "${fc_common_validation_absolute_sources}")

fc_configure_project_target(fusioncutter_common_validation)
fc_set_artifact_directories(fusioncutter_common_validation)

target_include_directories(fusioncutter_common_validation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/catalog"
    "${CMAKE_CURRENT_SOURCE_DIR}/config"
    "${CMAKE_CURRENT_SOURCE_DIR}/planning"
    "${CMAKE_CURRENT_SOURCE_DIR}/targets"
)

target_link_libraries(fusioncutter_common_validation PUBLIC
    FusionCutter::SDK
    FusionCutter::inih
    Zydis
)

# The private runtime archive owns every mutating and process-lifetime subsystem used only by the framework DLL.
add_library(fusioncutter_runtime STATIC
    patching/hook_registry.cpp
    patching/hook_registry.hpp
    patching/native_allocation.cpp
    patching/native_allocation.hpp
    patching/native_memory.cpp
    patching/native_memory.hpp
    patching/patch_preparation.cpp
    patching/patch_preparation.hpp
    patching/patch_transaction.cpp
    patching/patch_transaction.hpp
    reporting/reporting.cpp
    reporting/reporting.hpp
    reporting/status.cpp
    reporting/status.hpp
    reporting/crash_reporter.cpp
    reporting/crash_reporter.hpp
    reporting/tracing.cpp
    reporting/tracing.hpp
    runtime/core_runtime.cpp
    runtime/core_runtime.hpp
    runtime/interface_router.cpp
    runtime/interface_router.hpp
    runtime/installation.cpp
    runtime/installation.hpp
    runtime/late_images.cpp
    runtime/late_images.hpp
    runtime/patch_runtime.cpp
    runtime/patch_runtime.hpp
)

fc_configure_project_target(fusioncutter_runtime)
fc_set_artifact_directories(fusioncutter_runtime)

target_include_directories(fusioncutter_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/patching"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
)

target_link_libraries(fusioncutter_runtime PUBLIC
    fusioncutter_common_validation
    safetyhook::safetyhook
    quill::quill
    advapi32
)

# Built-in definitions and configured bundled plugins retain SDK state separately from common validation logic.
add_library(fusioncutter_builtin_contributions OBJECT
    ../core_plugin/core_plugin.cpp
)

fc_configure_project_target(fusioncutter_builtin_contributions)
fc_set_artifact_directories(fusioncutter_builtin_contributions)

target_include_directories(fusioncutter_builtin_contributions PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/catalog"
)

target_link_libraries(fusioncutter_builtin_contributions PUBLIC
    FusionCutter::SDK
)

fc_attach_configured_bundles(fusioncutter_builtin_contributions)

# The interface target composes both object owners without creating a second archive or registration path.
add_library(fusioncutter_catalog INTERFACE)
target_sources(fusioncutter_catalog INTERFACE
    $<TARGET_OBJECTS:fusioncutter_common_validation>
    $<TARGET_OBJECTS:fusioncutter_builtin_contributions>
)
get_property(configured_bundle_targets TARGET fusioncutter_builtin_contributions
    PROPERTY FC_CONFIGURED_BUNDLE_TARGETS)
foreach(configured_bundle_target IN LISTS configured_bundle_targets)
    target_sources(fusioncutter_catalog INTERFACE
        $<TARGET_OBJECTS:${configured_bundle_target}>
    )
endforeach()
target_link_libraries(fusioncutter_catalog INTERFACE
    fusioncutter_common_validation
    fusioncutter_builtin_contributions
)

# The universal framework DLL for each architecture exports only the immutable loader query table for ABI generation 1.
add_library(FusionCutter SHARED
    core_api.cpp
    dllmain.cpp
    fusioncutter.def
)

fc_configure_project_target(FusionCutter)
fc_set_artifact_directories(FusionCutter)

target_link_libraries(FusionCutter PRIVATE
    fusioncutter_catalog
    fusioncutter_runtime
)

set_target_properties(FusionCutter PROPERTIES
    OUTPUT_NAME FusionCutter
)
