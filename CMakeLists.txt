cmake_minimum_required(VERSION 3.28)

project(FusionCutter LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(fc_build)
include(fc_patches)
include(fc_vendor)

fc_initialize_build()
fc_add_format_targets()
fc_add_production_dependencies()

add_subdirectory(src/core)
add_subdirectory(src/loaders)
add_subdirectory(tools)

include(CTest)

if(BUILD_TESTING)
    fc_add_test_dependencies()
    add_subdirectory(tests)
endif()
