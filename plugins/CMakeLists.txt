cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterPlugins LANGUAGES C CXX)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/project.cmake")
fc_initialize_build()

if(NOT TARGET fusioncutter_patches)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../patches" "${CMAKE_CURRENT_BINARY_DIR}/patches")
endif()

# The runtime links the built-in plugin and its patches into each runtime DLL.
add_library(fusioncutter_core_plugin STATIC core.cpp)
fc_configure_project_target(fusioncutter_core_plugin)
target_link_libraries(fusioncutter_core_plugin PUBLIC fusioncutter_patches)
