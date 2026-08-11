if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(FusionCutterDInput8 LANGUAGES CXX)

    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../cmake")
    include(fc_build)
    fc_initialize_build()
endif()

if(NOT FC_ARCHITECTURE STREQUAL "x86")
    message(FATAL_ERROR "The DirectInput loader is available only in x86 builds.")
endif()

add_library(fusioncutter_dinput8 SHARED
    ../core_loader.cpp
    ../core_loader.hpp
    dinput8.cpp
    dinput8.def
)

target_include_directories(fusioncutter_dinput8 PRIVATE
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

fc_configure_project_target(fusioncutter_dinput8)
fc_set_artifact_directories(fusioncutter_dinput8)

# Windows reserves the four COM-style export names and warns when they also
# carry the system dinput8.dll ordinals that this proxy must preserve.
target_link_options(fusioncutter_dinput8 PRIVATE /IGNORE:4222)

set_target_properties(fusioncutter_dinput8 PROPERTIES
    OUTPUT_NAME dinput8
)
