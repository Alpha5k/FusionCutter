cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterPatches LANGUAGES C CXX)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/project.cmake")
fc_initialize_build()

if(NOT TARGET FusionCutter::SDK)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../sdk" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
endif()

# The core plugin and tests link these patch implementations from one static library.
add_library(fusioncutter_patches STATIC)
target_sources(fusioncutter_patches PRIVATE
    colored_chats/messages.cpp colored_chats/patch.cpp
    dedicated_fixes/patch.cpp
    lua_bindings/layout.cpp lua_bindings/native.cpp lua_bindings/patch.cpp
    lua_bindings/protected.c lua_bindings/frame.cpp lua_bindings/service.cpp lua_bindings/service_bindings.cpp
    lua_multiplayer/protocol.cpp lua_multiplayer/service.cpp lua_multiplayer/messages.cpp
    lua_multiplayer/script.cpp lua_multiplayer/patch.cpp
    rcon_server/patch.cpp
    rcon_server/aspyr/handler.cpp rcon_server/aspyr/layout.cpp
    rcon_server/gog/authentication.cpp rcon_server/gog/client.cpp rcon_server/gog/game.cpp
    rcon_server/gog/handler.cpp rcon_server/gog/layout.cpp
    rcon_server/gog/protocol.cpp rcon_server/gog/service.cpp
    unlock_frame_rate/patch.cpp
)
fc_configure_project_target(fusioncutter_patches)
set_target_properties(fusioncutter_patches PROPERTIES C_EXTENSIONS OFF C_STANDARD 11 C_STANDARD_REQUIRED ON)

target_link_libraries(fusioncutter_patches PUBLIC FusionCutter::SDK PRIVATE bcrypt ws2_32)
