include_guard(GLOBAL)

include(FetchContent)

# Stops configuration with setup instructions when a required vendored file is missing.
# relative_path is relative to FC_SOURCE_ROOT/vendor; include project.cmake first to set FC_SOURCE_ROOT.
function(fc_require_vendor_file relative_path dependency_name)
    if(NOT EXISTS "${FC_SOURCE_ROOT}/vendor/${relative_path}")
        message(FATAL_ERROR
            "Required files for ${dependency_name} are missing. Run git submodule update --init --recursive from the repository root, then configure again."
        )
    endif()
endfunction()

# Adds pinned SafetyHook with the Windows allocation granularity patch applied.
function(fc_add_safetyhook_dependency)
    if(TARGET safetyhook::safetyhook)
        return()
    endif()

    fc_require_vendor_file("SafetyHook/CMakeLists.txt" "SafetyHook")

    find_package(Git REQUIRED)

    # Patch a copy under the build directory. Its stamp tracks both the vendor revision and the correction.
    set(fc_safetyhook_source_dir "${CMAKE_CURRENT_BINARY_DIR}/vendor-source/SafetyHook")
    set(fc_safetyhook_patch "${FC_SOURCE_ROOT}/cmake/patches/safetyhook_windows_allocation_granularity.patch")
    file(SHA256 "${fc_safetyhook_patch}" fc_patch_digest)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FC_SOURCE_ROOT}/vendor/SafetyHook" rev-parse HEAD
        OUTPUT_VARIABLE fc_vendor_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    set(fc_source_stamp "${fc_vendor_revision}:${fc_patch_digest}")
    set(fc_previous_stamp "")
    if(EXISTS "${fc_safetyhook_source_dir}/fc-source-stamp")
        file(READ "${fc_safetyhook_source_dir}/fc-source-stamp" fc_previous_stamp)
    endif()

    # Refresh the copy whenever either input changes, then record the stamp after a successful patch.
    if(NOT fc_previous_stamp STREQUAL fc_source_stamp)
        file(REMOVE_RECURSE "${fc_safetyhook_source_dir}")
        file(COPY "${FC_SOURCE_ROOT}/vendor/SafetyHook/" DESTINATION "${fc_safetyhook_source_dir}" PATTERN ".git" EXCLUDE)

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${fc_safetyhook_patch}"
            WORKING_DIRECTORY "${fc_safetyhook_source_dir}"
            RESULT_VARIABLE fc_safetyhook_patch_result
            ERROR_VARIABLE fc_safetyhook_patch_error
        )
        if(NOT fc_safetyhook_patch_result EQUAL 0)
            message(FATAL_ERROR
                "The SafetyHook compatibility patch could not be applied to the pinned source: "
                "${fc_safetyhook_patch_error}"
            )
        endif()

        file(WRITE "${fc_safetyhook_source_dir}/fc-source-stamp" "${fc_source_stamp}")
    endif()

    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE)

    # CMake keeps the first FetchContent declaration, so both SafetyHook and validation use the pinned Zydis source.
    fc_add_zydis_dependency()

    set(SAFETYHOOK_BUILD_DOCS OFF)
    set(SAFETYHOOK_BUILD_TEST OFF)
    set(SAFETYHOOK_BUILD_EXAMPLES OFF)
    set(SAFETYHOOK_AMALGAMATE OFF)
    set(SAFETYHOOK_FETCH_ZYDIS ON)
    set(SAFETYHOOK_USE_CXXMODULES OFF)
    set(SAFETYHOOK_BUILD_MODULE OFF)

    add_subdirectory(
        "${fc_safetyhook_source_dir}"
        "${CMAKE_CURRENT_BINARY_DIR}/vendor/SafetyHook"
        EXCLUDE_FROM_ALL
    )
endfunction()

# Makes the pinned Zydis decoder available to native validation and SafetyHook.
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

# Adds the vendored test framework once for internal behavior tests.
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
