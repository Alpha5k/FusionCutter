cmake_minimum_required(VERSION 3.28)
project(FusionCutterExample LANGUAGES CXX)

# Configure with cmake -S example -B build -DCMAKE_PREFIX_PATH=<installed-sdk>, then cmake --build build.
# Run these commands in an MSVC developer shell whose architecture matches the SDK's verifier.
find_package(FusionCutter CONFIG REQUIRED)

add_library(ExamplePlugin MODULE plugin.cpp patch.cpp)
target_link_libraries(ExamplePlugin PRIVATE FusionCutter::SDK)
