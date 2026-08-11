#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::rcon_server::aspyr::layout {

// Hook and supporting proof sites.
inline constexpr std::uint32_t kCommandRva = 0x0025C7E0;
inline constexpr std::uint32_t kAuthenticatedCallRva = 0x00262313;
inline constexpr std::uint32_t kLuaWrapperRva = 0x003863A0;

// Game-owned RCON and Lua state used by the hook.
inline constexpr std::uint32_t kLuaStateRva = 0x009AFB38;
inline constexpr std::uint32_t kResponseBufferRva = 0x009C1450;
inline constexpr std::uint32_t kCommandDetailsRva = 0x009C13FE;
inline constexpr std::uint32_t kLoggedInRva = 0x009C13FF;

// Capacities are fixed by the corresponding game-owned buffers.
inline constexpr std::size_t kResponseCapacity = 0x1800;
inline constexpr std::size_t kCommandCapacity = 0x400;

inline constexpr auto kCommandPreimage =
    byte_array<0x44, 0x88, 0x44, 0x24, 0x18, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56>();

// The native TCP path sets both RCON context bytes, supplies output -1, and then calls RconManager.
inline constexpr auto kAuthenticatedCallPreimage =
    byte_array<0x45, 0x33, 0xC9, 0xC6, 0x05, 0xE2, 0xF0, 0x75, 0x00, 0x01, 0x45, 0x33, 0xC0, 0xC6, 0x05, 0xD7, 0xF0,
               0x75, 0x00, 0x01, 0x48, 0x8D, 0x94, 0x24, 0x50, 0x04, 0x00, 0x00, 0x41, 0x8D, 0x49, 0xFF, 0xE8, 0xA8,
               0xA4, 0xFF, 0xFF>();

inline constexpr auto kLuaWrapperPreimage =
    byte_array<0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0x54, 0x24, 0x20, 0x48>();

} // namespace fusioncutter::patches::rcon_server::aspyr::layout
