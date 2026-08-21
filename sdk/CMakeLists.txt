# The header-only SDK target carries the exact ABI include roots and compiler policy into every plugin build.
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
    /W4
)

# Observation of shared hooks preserves both ambient Windows error channels around independently compiled callbacks.
target_link_libraries(fc_sdk INTERFACE
    ws2_32
)

target_include_directories(fc_sdk INTERFACE
    "$<BUILD_INTERFACE:${FC_SOURCE_ROOT}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FusionCutterPlugin.cmake")

include(CMakePackageConfigHelpers)

# Package installation exports the same interface target and bridge helper used by in-tree plugin fixtures.
set(FC_SDK_INSTALL_CMAKEDIR "lib/cmake/FusionCutterSDK")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FusionCutterSDKConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FusionCutterSDKConfig.cmake"
    INSTALL_DESTINATION "${FC_SDK_INSTALL_CMAKEDIR}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/FusionCutterSDKConfigVersion.cmake"
    VERSION 2.0.0
    COMPATIBILITY SameMajorVersion
)

install(TARGETS fc_sdk EXPORT FusionCutterSDKTargets)

install(DIRECTORY "${FC_SOURCE_ROOT}/include/FusionCutter"
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/FusionCutter"
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

install(EXPORT FusionCutterSDKTargets
    FILE FusionCutterSDKTargets.cmake
    NAMESPACE FusionCutter::
    DESTINATION "${FC_SDK_INSTALL_CMAKEDIR}"
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/FusionCutterSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/FusionCutterSDKConfigVersion.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FusionCutterPlugin.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FusionCutterPluginBridge.cpp.in"
    DESTINATION "${FC_SDK_INSTALL_CMAKEDIR}"
)
