# Build the offline verifier in the root project or as a standalone tools project.
cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterTools LANGUAGES C CXX)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/project.cmake")
fc_initialize_build()

# Reuse the root project's runtime target, or add it for a standalone tools build.
if(NOT TARGET fusioncutter_runtime)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../runtime" "${CMAKE_CURRENT_BINARY_DIR}/runtime")
endif()

add_executable(fusioncutter_verify
    verify.cpp
)

fc_configure_project_target(fusioncutter_verify)

target_link_libraries(fusioncutter_verify PRIVATE
    fusioncutter_runtime
)

set_target_properties(fusioncutter_verify PROPERTIES
    OUTPUT_NAME FusionCutterVerify
)

fc_install_binary(fusioncutter_verify SDK tools)
fc_install_delivery_info(SDK TRUE)
