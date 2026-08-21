# The raw startup host keeps one stable PE layout across build configurations so its test profile is exact.
# Its CRT-free Debug build cannot retain the default runtime check library dependency.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
add_executable(fc_slice_host slice_host.c)
set_target_properties(fc_slice_host PROPERTIES
    C_EXTENSIONS OFF
    MSVC_RUNTIME_CHECKS ""
    OUTPUT_NAME FusionCutterSliceHost
)
target_compile_features(fc_slice_host PRIVATE c_std_17)
target_compile_definitions(fc_slice_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(fc_slice_host PRIVATE /GS- /Od /W4)
target_link_options(fc_slice_host PRIVATE
    /NODEFAULTLIB
    /INCREMENTAL:NO
    /OPT:NOREF
    /OPT:NOICF
    /FIXED
    /DYNAMICBASE:NO
    /SUBSYSTEM:CONSOLE
    /ENTRY:fc_slice_entry
)
target_link_libraries(fc_slice_host PRIVATE kernel32 ws2_32)
target_include_directories(fc_slice_host PRIVATE
    "${FC_SOURCE_ROOT}/include"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices/simple_write"
)
fc_set_artifact_directories(fc_slice_host)

# A fixed, CRT-free DLL gives the x86 loader pump one deterministic image that appears after startup installation.
add_library(fc_slice_late_image SHARED slice_late_image.c)
set_target_properties(fc_slice_late_image PROPERTIES
    C_EXTENSIONS OFF
    MSVC_RUNTIME_CHECKS ""
    OUTPUT_NAME FusionCutterSlicePeer
)
target_compile_features(fc_slice_late_image PRIVATE c_std_17)
target_compile_definitions(fc_slice_late_image PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(fc_slice_late_image PRIVATE /GS- /Od /W4)
target_link_options(fc_slice_late_image PRIVATE
    /NODEFAULTLIB
    /INCREMENTAL:NO
    /OPT:NOREF
    /OPT:NOICF
    /FIXED
    /DYNAMICBASE:NO
    /NOENTRY
)
target_link_libraries(fc_slice_late_image PRIVATE kernel32)
target_include_directories(fc_slice_late_image PRIVATE
    "${FC_SOURCE_ROOT}/include"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices/simple_write"
)
fc_set_artifact_directories(fc_slice_late_image)

# Recompile the one shared non-mutating owner only to add the private, exact slice host recognition profile.
get_property(fc_slice_common_sources GLOBAL PROPERTY FC_COMMON_VALIDATION_SOURCES)
if(NOT fc_slice_common_sources)
    message(FATAL_ERROR "The Stage 10 native harness requires the common validation source authority.")
endif()
add_library(fc_slice_common_validation OBJECT ${fc_slice_common_sources})
fc_configure_project_target(fc_slice_common_validation)
target_compile_definitions(fc_slice_common_validation PRIVATE FC_SLICE_TEST_PROFILE)
target_include_directories(fc_slice_common_validation PUBLIC
    "${FC_SOURCE_ROOT}/src/core/catalog"
    "${FC_SOURCE_ROOT}/src/core/config"
    "${FC_SOURCE_ROOT}/src/core/planning"
    "${FC_SOURCE_ROOT}/src/core/targets"
)
target_link_libraries(fc_slice_common_validation PUBLIC
    FusionCutter::SDK
    FusionCutter::inih
    Zydis
)

# The external route has no configured bundles; its table remains an ordinary empty contribution source.
add_library(fc_slice_external_contributions OBJECT
    "${FC_SOURCE_ROOT}/src/core_plugin/core_plugin.cpp"
    empty_configured_bundles.cpp
)
fc_configure_project_target(fc_slice_external_contributions)
target_include_directories(fc_slice_external_contributions PRIVATE
    "${FC_SOURCE_ROOT}/src/core/catalog"
)
target_link_libraries(fc_slice_external_contributions PRIVATE FusionCutter::SDK)

# Links one framework DLL used only by tests while retaining the production runtime and CoreApi entrypoint.
function(fc_add_slice_core target contributions)
    add_library(${target} SHARED
        "${FC_SOURCE_ROOT}/src/core/core_api.cpp"
        "${FC_SOURCE_ROOT}/src/core/dllmain.cpp"
        "${FC_SOURCE_ROOT}/src/core/fusioncutter.def"
        $<TARGET_OBJECTS:fc_slice_common_validation>
        $<TARGET_OBJECTS:${contributions}>
        ${ARGN}
    )
    fc_configure_project_target(${target})
    target_include_directories(${target} PRIVATE "${FC_SOURCE_ROOT}/src/core")
    # Link the runtime archive directly so its standard profile target does not accompany the test profile objects.
    target_link_libraries(${target} PRIVATE
        "$<TARGET_FILE:fusioncutter_runtime>"
        FusionCutter::SDK
        FusionCutter::inih
        Zydis
        safetyhook::safetyhook
        quill::quill
        advapi32
    )
    add_dependencies(${target} fusioncutter_runtime ${contributions})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME FusionCutter
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/${target}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/${target}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/${target}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/${target}"
    )
endfunction()

fc_add_slice_core(fc_slice_external_core fc_slice_external_contributions)
fc_add_slice_core(fc_slice_bundled_core fc_simple_write_bundle_contributions
    $<TARGET_OBJECTS:fc_simple_write_slice_bundle>
)
