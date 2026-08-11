if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(FusionCutterRconServer LANGUAGES CXX)

    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../cmake")
    include(fc_build)
    fc_initialize_build()
endif()

add_library(fusioncutter_rconserver SHARED
    ../core_loader.cpp
    ../core_loader.hpp
    rconserver.cpp
)

target_include_directories(fusioncutter_rconserver PRIVATE
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

fc_configure_project_target(fusioncutter_rconserver)
fc_set_artifact_directories(fusioncutter_rconserver)

if(FC_ARCHITECTURE STREQUAL "x86")
    set(rconserver_output_name RconServer_32)
else()
    set(rconserver_output_name RconServer_64)
endif()

set_target_properties(fusioncutter_rconserver PROPERTIES
    OUTPUT_NAME "${rconserver_output_name}"
)
