cmake_minimum_required(VERSION 3.28)
project(FusionCutter LANGUAGES C CXX)

include(CTest)

# Add the SDK first so component projects reuse its target.
add_subdirectory(sdk)
add_subdirectory(runtime)
add_subdirectory(loaders)
add_subdirectory(tools)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
