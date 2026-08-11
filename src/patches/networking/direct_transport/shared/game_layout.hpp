#pragma once

#include <FusionCutter/patching.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::direct_transport {

// Describes every native game site and helper used by the shared transport hooks.
struct GameLayout {
    std::uint32_t final_send_rva;
    std::uint32_t group_send_rva;
    std::uint32_t receive_rva;
    std::uint32_t intake_rva;
    std::uint32_t disconnect_rva;
    std::uint32_t reset_rva;
    std::uint32_t remote_member_callback_rva;
    std::uint32_t remote_member_listener_rva;
    std::uint32_t local_lobby_left_callback_rva;
    std::uint32_t local_lobby_left_listener_rva;
    std::uint32_t get_matchmaking_rva;
    std::array<std::byte, 21> get_matchmaking_preimage;
    std::uint32_t get_networking_rva;
    std::array<std::byte, 21> get_networking_preimage;
    std::uint32_t endpoint_table_rva;
    std::uint32_t packet_allocate_rva;
    std::array<std::byte, 16> packet_allocate_preimage;
    std::uint32_t packet_initialize_rva;
    std::array<std::byte, 16> packet_initialize_preimage;
    std::uint32_t packet_prefix_rva;
};

inline constexpr std::uint32_t kPreferredImageBase = 0x0040'0000;

inline constexpr auto kFinalSendPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57, 0x8B, 0xFA, 0x8B, 0xF1>();
inline constexpr auto kGroupSendPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x89, 0x55, 0xF8, 0x33, 0xF6>();
inline constexpr auto kReceivePreimage = byte_array<0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56>();
inline constexpr auto kIntakePreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0xDA, 0x89, 0x5D, 0xFC, 0x0F, 0xBE>();
inline constexpr auto kDisconnectPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0x53, 0x8B, 0xD9, 0xB9>();
inline constexpr auto kResetPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x08, 0x53, 0x56, 0x8A, 0xD9>();
inline constexpr auto kRemoteMemberCallbackPreimage = byte_array<0xC2, 0x0C, 0x00>();
inline constexpr auto kLocalLobbyLeftCallbackPreimage = byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8>();

// Relocates absolute operands in target preimages before native-site validation.
[[nodiscard]] std::array<std::byte, 21> relocated_getter_preimage(const GameLayout& layout, std::uintptr_t image_base,
                                                                  bool networking) noexcept;
[[nodiscard]] std::array<std::byte, 16> relocated_packet_preimage(const GameLayout& layout, std::uintptr_t image_base,
                                                                  bool initialize) noexcept;

} // namespace fusioncutter::patches::direct_transport
