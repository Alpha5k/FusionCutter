cmake_minimum_required(VERSION 3.28)

project(FusionCutter LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(fc_build)
include(fc_vendor)
include(fc_tests)

# Define shared build policy and the lightweight dependencies required by common validation.
fc_initialize_build()
fc_add_format_targets()

# Production and packaged-testing targets consume their reviewed dependencies from the same configured build.
fc_add_production_dependencies()
fc_add_test_dependencies()
add_subdirectory(sdk)

include(fc_bundles)

set(FC_BUNDLE_FILE "" CACHE FILEPATH "Explicit CMake file that declares bundled Fusion Cutter plugins")
if(FC_BUNDLE_FILE)
    # Bundled plugin declarations are opt-in and evaluated before the owning framework targets are materialized.
    get_filename_component(fc_bundle_file_absolute "${FC_BUNDLE_FILE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${fc_bundle_file_absolute}")
        message(FATAL_ERROR "FC_BUNDLE_FILE does not exist: ${fc_bundle_file_absolute}")
    endif()
    include("${fc_bundle_file_absolute}")
endif()

add_subdirectory(src/core)
add_subdirectory(src/loaders)
add_subdirectory(tools/verify)
add_subdirectory(sdk/testing)

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
