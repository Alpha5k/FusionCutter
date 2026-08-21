include_guard(GLOBAL)

get_filename_component(FC_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Establishes process-wide toolchain, runtime, diagnostic, and architecture policy before targets are declared.
function(fc_initialize_build)
    if(NOT WIN32 OR NOT MSVC)
        message(FATAL_ERROR "Fusion Cutter currently requires Windows and the Visual Studio 2022 MSVC toolchain.")
    endif()

    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" PARENT_SCOPE)

    set(FC_WARNINGS_AS_ERRORS OFF CACHE BOOL "Treat warnings in project-owned source as errors")
    set(FC_VERSION_STRING "development" CACHE STRING "Version written to Fusion Cutter diagnostics")
    set(FC_BUILD_ID "local" CACHE STRING "Stable identifier used to match diagnostics to developer symbols")

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(FC_ARCHITECTURE "x86" CACHE INTERNAL "Fusion Cutter target architecture")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(FC_ARCHITECTURE "x64" CACHE INTERNAL "Fusion Cutter target architecture")
    else()
        message(FATAL_ERROR "Fusion Cutter supports only x86 and x64 builds.")
    endif()

    message(STATUS "Fusion Cutter architecture: ${FC_ARCHITECTURE}")
endfunction()

# Applies the warnings, language mode, and link optimization policy shared by compiled project targets.
function(fc_configure_project_target target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
    )

    target_compile_definitions(${target} PRIVATE
        FC_BUILD_ID="${FC_BUILD_ID}"
        FC_VERSION_STRING="${FC_VERSION_STRING}"
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    target_compile_options(${target} PRIVATE
        /EHsc
        /permissive-
        /W4
    )

    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE" OR
       target_type STREQUAL "SHARED_LIBRARY" OR
       target_type STREQUAL "MODULE_LIBRARY")
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>"
            "$<$<CONFIG:RelWithDebInfo>:/OPT:REF>"
            "$<$<CONFIG:RelWithDebInfo>:/OPT:ICF>"
        )
    endif()

    if(FC_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE /WX)
    endif()
endfunction()

# Keeps binary and symbol outputs for every build configuration under one predictable artifact root.
function(fc_set_artifact_directories target)
    set(output_directory "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>")
    set_target_properties(${target} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${output_directory}"
        LIBRARY_OUTPUT_DIRECTORY "${output_directory}"
        PDB_OUTPUT_DIRECTORY "${output_directory}"
        RUNTIME_OUTPUT_DIRECTORY "${output_directory}"
    )
endfunction()

# Exposes the repository formatting script through generator-independent developer targets.
function(fc_add_format_targets)
    find_program(FC_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)

    add_custom_target(format
        COMMAND "${FC_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${FC_SOURCE_ROOT}/tools/format.ps1"
        WORKING_DIRECTORY "${FC_SOURCE_ROOT}"
        VERBATIM
    )

    add_custom_target(format-check
        COMMAND "${FC_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${FC_SOURCE_ROOT}/tools/format.ps1" -Check
        WORKING_DIRECTORY "${FC_SOURCE_ROOT}"
        VERBATIM
    )
endfunction()
