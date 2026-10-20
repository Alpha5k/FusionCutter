cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterRuntime LANGUAGES C CXX)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/project.cmake")
fc_initialize_build()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/dependencies.cmake")
fc_add_safetyhook_dependency()

if(NOT TARGET FusionCutter::SDK)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../sdk" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
endif()

if(NOT TARGET fusioncutter_core_plugin)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../plugins" "${CMAKE_CURRENT_BINARY_DIR}/plugins")
endif()

# Share the runtime implementation between FusionCutter.dll, the dedicated server DLL and validation tools.
add_library(fusioncutter_runtime STATIC
    src/diagnostic.hpp
    src/fatal_boundary.cpp src/fatal_boundary.hpp
    src/plugins/validation.cpp src/plugins/loading.cpp src/plugins/plugin.hpp
    src/images/image.cpp src/images/image.hpp targets/image_profiles.cpp
    src/patches/selection.cpp src/patches/selection.hpp
    src/patches/dependency.cpp src/patches/dependency.hpp
    src/configuration/configuration.cpp src/configuration/configuration.hpp
    src/configuration/ini.cpp src/configuration/ini.hpp
    src/configuration/storage.cpp src/configuration/storage.hpp
    src/configuration/runtime_settings.cpp
    src/patches/patch_record.cpp src/patches/patch_record.hpp
    src/native/plan.cpp src/native/plan.hpp
    src/native/plan_detail.hpp src/native/operations.cpp src/native/validation.cpp
    src/native/installation.cpp src/native/resource_api.cpp
    src/patches/patch_api.cpp src/patches/patch_api.hpp
    src/patches/installation.cpp src/patches/installation.hpp
    src/patches/late_images.cpp src/patches/late_images.hpp
    src/core_runtime.cpp src/core_runtime.hpp
    src/game.cpp src/game.hpp src/game_api.cpp
    src/multiplayer.cpp src/multiplayer.hpp src/multiplayer_api.cpp src/multiplayer_messages.cpp
    src/message_protocol.cpp src/message_protocol.hpp
    src/lobby.cpp src/lobby.hpp src/lobby_api.cpp
    targets/game_context.cpp targets/game_context.hpp
    targets/galaxy.cpp targets/galaxy.hpp targets/galaxy_calls.cpp targets/galaxy_calls.hpp
    src/offline_validation.cpp src/offline_validation.hpp
    src/reporting/reporting.cpp src/reporting/reporting.hpp
    src/reporting/log_writer.cpp src/reporting/log_writer.hpp
    src/reporting/runtime_reporting.cpp
    src/reporting/status.cpp src/reporting/status.hpp
    src/reporting/tracing.cpp src/reporting/tracing.hpp
    src/reporting/trace_channel.cpp src/reporting/trace_channel.hpp
    src/reporting/crash_reporter.cpp src/reporting/crash_reporter.hpp
    src/reporting/crash_report.cpp src/reporting/crash_report.hpp
    src/reporting/crash_memory.cpp src/reporting/exception_capture.cpp
    src/reporting/bounded_text.hpp src/reporting/byte_ring.hpp
    src/native/bridge.cpp src/native/bridge.hpp
    src/native/bridge_layout.cpp src/native/bridge_layout.hpp
    src/native/native_call.cpp src/native/native_call.hpp
    src/native/native_address.cpp
    src/native/native_allocation.cpp
    src/native/native_memory.cpp
    src/native/hook_registry.cpp src/native/hook_registry.hpp
    src/native/hook_dispatch.cpp src/native/hook_site.hpp)

fc_configure_project_target(fusioncutter_runtime)
target_include_directories(fusioncutter_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Multiplayer connection tokens use the Windows cryptographic random generator in bcrypt.
target_link_libraries(fusioncutter_runtime PUBLIC FusionCutter::SDK safetyhook::safetyhook fusioncutter_core_plugin ws2_32 advapi32 bcrypt)

add_library(FusionCutter SHARED src/core_api.cpp src/dllmain.cpp src/fusioncutter.def)
fc_configure_project_target(FusionCutter)

target_link_libraries(FusionCutter PRIVATE fusioncutter_runtime)

fc_install_binary(FusionCutter Client)
fc_install_delivery_info(Client TRUE)
fc_install_delivery_info(Runtime TRUE)
fc_install_delivery_info(Symbols TRUE)
