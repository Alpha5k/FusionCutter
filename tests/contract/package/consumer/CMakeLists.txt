cmake_minimum_required(VERSION 3.28)

project(FusionCutterPackageConsumer LANGUAGES CXX)

find_package(FusionCutterSDK CONFIG REQUIRED)

fc_add_plugin(PackageConsumer
    FACTORY package_consumer::build_plugin
    SOURCES plugin.cpp
)

# The clean prefix must also provide the compiled scenario archive, packaged Catch2 main, and staging helper.
fc_add_plugin_tests(PackageConsumerTests
    PLUGINS
        PackageConsumer
    SOURCES
        testing.cpp
)
