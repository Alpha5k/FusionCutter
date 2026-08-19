cmake_minimum_required(VERSION 3.28)

project(FusionCutter LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(fc_build)

fc_initialize_build()
fc_add_format_targets()

include(CTest)
