fc_add_plugin(fc_simple_write_slice
    FACTORY fc::fixtures::simple_write::build_plugin
    SOURCES simple_write.cpp
)

# The identical factory source also enters a bundled bridge for the custom slice framework built by the native harness.
fc_bundle_plugin(fc_simple_write_slice_bundle
    FACTORY fc::fixtures::simple_write::build_plugin
    SOURCES simple_write.cpp
)

# A second contribution owner lets public Scenario exercise the bundled route without changing the standard framework.
add_library(fc_simple_write_bundle_contributions OBJECT
    "${FC_SOURCE_ROOT}/src/core_plugin/core_plugin.cpp"
)
fc_configure_project_target(fc_simple_write_bundle_contributions)
target_include_directories(fc_simple_write_bundle_contributions PRIVATE
    "${FC_SOURCE_ROOT}/src/core/catalog"
)
target_link_libraries(fc_simple_write_bundle_contributions PRIVATE FusionCutter::SDK)
fc_attach_configured_bundles(fc_simple_write_bundle_contributions)

# This test-only archive exposes the public Testing API with the custom bundle table linked into its normal owner.
add_library(fc_simple_write_bundle_testing STATIC
    "${FC_SOURCE_ROOT}/sdk/testing/testing.cpp"
    $<TARGET_OBJECTS:fusioncutter_common_validation>
    $<TARGET_OBJECTS:fc_simple_write_bundle_contributions>
    $<TARGET_OBJECTS:fc_simple_write_slice_bundle>
)
fc_configure_project_target(fc_simple_write_bundle_testing)
fc_set_artifact_directories(fc_simple_write_bundle_testing)
target_include_directories(fc_simple_write_bundle_testing PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
)
target_link_libraries(fc_simple_write_bundle_testing PUBLIC
    FusionCutter::SDK
    FusionCutter::inih
    Zydis
)
