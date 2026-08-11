if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(FusionCutterBattlefront2 LANGUAGES CXX)

    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../cmake")
    include(fc_build)
    fc_initialize_build()
endif()

if(NOT FC_ARCHITECTURE STREQUAL "x64")
    message(FATAL_ERROR "The Battlefront2 loader is available only in x64 builds.")
endif()

add_library(fusioncutter_battlefront2 SHARED
    ../core_loader.cpp
    ../core_loader.hpp
    battlefront2.cpp
    battlefront2.def
)

target_include_directories(fusioncutter_battlefront2 PRIVATE
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

fc_configure_project_target(fusioncutter_battlefront2)
fc_set_artifact_directories(fusioncutter_battlefront2)

set_target_properties(fusioncutter_battlefront2 PROPERTIES
    OUTPUT_NAME Battlefront2
)
