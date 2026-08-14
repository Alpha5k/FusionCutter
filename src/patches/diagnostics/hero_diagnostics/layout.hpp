#pragma once

#include <FusionCutter/patching.hpp>

#include <cstdint>

namespace fusioncutter::patches::hero_diagnostics {

struct ClientLayout {
    NativeSite<12> read;
    NativeSite<16> exit_state;
    NativeSite<16> action_state;
    NativeSite<16> setup_pose;
    NativeSite<16> override_controls;
    NativeSite<16> override_velocity;
    NativeSite<13> sound_play;
    NativeSite<5> player_move_parsed;
    NativeSite<16> deflect;
    std::uint32_t host_turn_rva;
    std::uint32_t client_turn_rva;
    std::uint32_t update_turn_rva;
    std::uint32_t predict_turn_rva;
    std::uint32_t acknowledged_turn_rva;
    std::uint32_t is_local_turn_rva;
    std::uint32_t is_update_turn_rva;
    std::uint32_t rollback_rva;
    std::uint32_t network_enabled_rva;
    std::uint32_t network_client_active_rva;
    std::uint32_t outer_delta_rva;
    std::uint32_t get_joystick_index_rva;
    std::uint32_t remote_moves_rva;
    std::uint32_t destination_rva;
};

struct ServerLayout {
    NativeSite<16> update;
    NativeSite<16> enter_state;
    NativeSite<16> exit_state;
    NativeSite<16> override_velocity;
    NativeSite<12> write;
    NativeSite<16> deflect;
    std::uint32_t host_turn_rva;
    std::uint32_t destination_rva;
    std::uint32_t move_history_rva;
};

[[nodiscard]] const ClientLayout& client_layout_for(TargetLayout target) noexcept;
[[nodiscard]] const ServerLayout& server_layout() noexcept;

} // namespace fusioncutter::patches::hero_diagnostics
