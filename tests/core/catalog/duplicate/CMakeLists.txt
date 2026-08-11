cmake_minimum_required(VERSION 3.28)

project(FusionCutterDuplicatePatchTest LANGUAGES NONE)

list(APPEND CMAKE_MODULE_PATH "${FC_PROJECT_ROOT}/cmake")
include(fc_patches)

set(FC_ARCHITECTURE x86)
set(FC_CORE_ROLE Universal)

fc_generate_patch_catalog(
    PATCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/patches"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated.cpp"
    OUT_SOURCES patch_sources
)
