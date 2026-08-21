include_guard(GLOBAL)

include(FetchContent)

# Fails configuration before a dependency can silently fall back to an unreviewed system or network copy.
function(fc_require_vendor_file relative_path dependency_name)
    if(NOT EXISTS "${FC_SOURCE_ROOT}/vendor/${relative_path}")
        message(FATAL_ERROR
            "${dependency_name} is unavailable. Initialize the vendor submodules recursively before configuring."
        )
    endif()
endfunction()

# Configures the complete reviewed production dependency graph with examples, tests, and installation disabled.
function(fc_add_production_dependencies)
    fc_require_vendor_file("SafetyHook/CMakeLists.txt" "SafetyHook")
    fc_require_vendor_file("Zydis/CMakeLists.txt" "Zydis")
    fc_require_vendor_file("Zydis/dependencies/zycore/CMakeLists.txt" "Zycore")
    fc_require_vendor_file("Quill/CMakeLists.txt" "Quill")

    fc_add_safetyhook_dependency()

    set(QUILL_BUILD_BENCHMARKS OFF)
    set(QUILL_BUILD_EXAMPLES OFF)
    set(QUILL_BUILD_FUZZING OFF)
    set(QUILL_BUILD_MODULE OFF)
    set(QUILL_BUILD_TESTS OFF)
    set(QUILL_DISABLE_NON_PREFIXED_MACROS ON)
    set(QUILL_DOCS_GEN OFF)
    set(QUILL_ENABLE_INSTALL OFF)

    add_subdirectory(
        "${FC_SOURCE_ROOT}/vendor/Quill"
        "${CMAKE_CURRENT_BINARY_DIR}/vendor/Quill"
        EXCLUDE_FROM_ALL
    )

    fc_add_inih_dependency()
endfunction()

# Configures SafetyHook independently so common validators do not inherit the full production graph.
function(fc_add_safetyhook_dependency)
    if(TARGET safetyhook::safetyhook)
        return()
    endif()
    fc_require_vendor_file("SafetyHook/CMakeLists.txt" "SafetyHook")

    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE)

    # Record the reviewed local source before SafetyHook declares Zydis through FetchContent. CMake's first
    # declaration wins, so SafetyHook cannot download or substitute another revision during configuration.
    fc_add_zydis_dependency()

    set(SAFETYHOOK_BUILD_DOCS OFF)
    set(SAFETYHOOK_BUILD_TEST OFF)
    set(SAFETYHOOK_BUILD_EXAMPLES OFF)
    set(SAFETYHOOK_AMALGAMATE OFF)
    set(SAFETYHOOK_FETCH_ZYDIS ON)
    set(SAFETYHOOK_USE_CXXMODULES OFF)
    set(SAFETYHOOK_BUILD_MODULE OFF)

    add_subdirectory(
        "${FC_SOURCE_ROOT}/vendor/SafetyHook"
        "${CMAKE_CURRENT_BINARY_DIR}/vendor/SafetyHook"
        EXCLUDE_FROM_ALL
    )
endfunction()

# Common hook validation needs only the reviewed decoder; physical hook/runtime dependencies are configured separately.
function(fc_add_zydis_dependency)
    if(TARGET Zydis)
        return()
    endif()
    fc_require_vendor_file("Zydis/CMakeLists.txt" "Zydis")
    fc_require_vendor_file("Zydis/dependencies/zycore/CMakeLists.txt" "Zycore")

    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE)
    set(ZYDIS_BUILD_EXAMPLES OFF)
    set(ZYDIS_BUILD_TOOLS OFF)
    set(ZYDIS_BUILD_DOXYGEN OFF)
    set(ZYDIS_BUILD_TESTS OFF)
    FetchContent_Declare(Zydis
        SOURCE_DIR "${FC_SOURCE_ROOT}/vendor/Zydis"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/vendor/Zydis"
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(Zydis)
endfunction()

# Defines the one inih build and the macros that configure its parser without forcing unrelated production
# dependencies.
function(fc_add_inih_dependency)
    if(TARGET fc_inih)
        return()
    endif()
    fc_require_vendor_file("inih/ini.c" "inih")

    add_library(fc_inih STATIC
        "${FC_SOURCE_ROOT}/vendor/inih/ini.c"
    )
    add_library(FusionCutter::inih ALIAS fc_inih)

    target_include_directories(fc_inih PUBLIC
        "${FC_SOURCE_ROOT}/vendor/inih"
    )

    target_compile_definitions(fc_inih PUBLIC
        INI_ALLOW_MULTILINE=0
        INI_ALLOW_BOM=1
        INI_ALLOW_INLINE_COMMENTS=0
        INI_STOP_ON_FIRST_ERROR=1
        INI_HANDLER_LINENO=1
        INI_CALL_HANDLER_ON_NEW_SECTION=1
        INI_ALLOW_NO_VALUE=0
        INI_USE_STACK=1
        INI_MAX_LINE=4099
    )

    set_target_properties(fc_inih PROPERTIES
        C_EXTENSIONS OFF
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
    )
endfunction()

# Adds the vendored test framework once for in-tree tests and packaged Testing consumers.
function(fc_add_test_dependencies)
    if(TARGET Catch2::Catch2WithMain)
        return()
    endif()
    fc_require_vendor_file("Catch2/CMakeLists.txt" "Catch2")

    set(CATCH_INSTALL_DOCS OFF)
    set(CATCH_INSTALL_EXTRAS OFF)

    add_subdirectory(
        "${FC_SOURCE_ROOT}/vendor/Catch2"
        "${CMAKE_CURRENT_BINARY_DIR}/vendor/Catch2"
        EXCLUDE_FROM_ALL
    )
endfunction()
