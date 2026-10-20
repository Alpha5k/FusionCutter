cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterSDK LANGUAGES C CXX)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/project.cmake")
fc_initialize_build()

# Plugins link this interface target to obtain the SDK headers and required compiler settings.
add_library(fc_sdk INTERFACE)
add_library(FusionCutter::SDK ALIAS fc_sdk)

set_target_properties(fc_sdk PROPERTIES EXPORT_NAME SDK)

target_compile_features(fc_sdk INTERFACE cxx_std_23)

target_compile_definitions(fc_sdk INTERFACE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(fc_sdk INTERFACE
    /EHsc
    /permissive-
    "$<$<CONFIG:Debug>:/MTd>"
    "$<$<NOT:$<CONFIG:Debug>>:/MT>"
)

target_include_directories(fc_sdk INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)

include(CMakePackageConfigHelpers)

# Export the headers and interface target as a package usable through find_package(FusionCutter).
set(FC_SDK_INSTALL_CMAKEDIR "lib/cmake/FusionCutter")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FusionCutterSDKConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FusionCutterConfig.cmake"
    INSTALL_DESTINATION "${FC_SDK_INSTALL_CMAKEDIR}"
)

install(TARGETS fc_sdk
    EXPORT FusionCutterTargets
    COMPONENT SDK
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/fc"
    DESTINATION include
    COMPONENT SDK
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
)

install(EXPORT FusionCutterTargets
    FILE FusionCutterTargets.cmake
    NAMESPACE FusionCutter::
    DESTINATION "${FC_SDK_INSTALL_CMAKEDIR}"
    COMPONENT SDK
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/FusionCutterConfig.cmake"
    DESTINATION "${FC_SDK_INSTALL_CMAKEDIR}"
    COMPONENT SDK
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/example/" DESTINATION example COMPONENT SDK
    FILES_MATCHING PATTERN "CMakeLists.txt" PATTERN "*.cpp")

# Direct SDK builds produce the verifier shipped with its headers and maintained example.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../tools" "${CMAKE_CURRENT_BINARY_DIR}/tools")
endif()
