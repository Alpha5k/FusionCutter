#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::rcon_server::gog::layout {

// Patched or invoked code sites.
inline constexpr std::uint32_t kCommandRva = 0x001B0030;
inline constexpr std::uint32_t kChatCallRva = 0x001B2F67;
inline constexpr std::uint32_t kChatOperandRva = kChatCallRva + 2;
inline constexpr std::uint32_t kSnprintfImportRva = 0x0036C248;
inline constexpr std::uint32_t kLuaLoadBufferRva = 0x0029C0C0;
inline constexpr std::uint32_t kLuaPcallRva = 0x0029CF40;
inline constexpr std::uint32_t kLuaSetTopRva = 0x0029D490;

// Game-owned RCON and server state read by the service worker.
inline constexpr std::uint32_t kResponseBufferRva = 0x01BA39D0;
inline constexpr std::uint32_t kCommandDetailsRva = 0x01A58EBC;
inline constexpr std::uint32_t kAdminPasswordRva = 0x01A64330;
inline constexpr std::uint32_t kLoggedInRva = 0x01B9C2E2;
inline constexpr std::uint32_t kGamePortRva = 0x003E9EF4;
inline constexpr std::uint32_t kIdleRva = 0x01A58EBD;
inline constexpr std::uint32_t kTeamArrayRva = 0x003EAAA0;
inline constexpr std::uint32_t kMapStatusRva = 0x01AB1054;
inline constexpr std::uint32_t kLuaStateRva = 0x01A58E50;

// Capacities are fixed by the corresponding game-owned buffers.
inline constexpr std::size_t kResponseCapacity = 0x1800;
inline constexpr std::size_t kAdminPasswordCapacity = 0x10;
inline constexpr std::size_t kCommandCapacity = 0x100;
inline constexpr std::uint8_t kMapIdle = 0;

inline constexpr auto kCommandPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x28, 0x03, 0x00, 0x00>();

// This site is an indirect snprintf import call. The operand rewrite separately proves the expected IAT slot.
inline constexpr auto kChatCallPreimage = byte_array<0xFF, 0x15>();

inline constexpr auto kLuaLoadBufferPreimage = byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45, 0x0C, 0xFF>();
inline constexpr auto kLuaPcallPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x14, 0x83, 0xEC, 0x08, 0x56, 0x57>();
inline constexpr auto kLuaSetTopPreimage = byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x0C, 0x85, 0xD2>();

} // namespace fusioncutter::patches::rcon_server::gog::layout
