#pragma once

#include <FusionCutter/patching.hpp>

#include <array>
#include <cstdint>

namespace fusioncutter::patches::weapon_swap_replay {

// Identifies the network frontiers and input history that define replay ownership.
struct WeaponSwapStateLayout {
    std::uint32_t update_turn_rva;
    std::uint32_t predict_turn_rva;
    std::uint32_t acknowledged_turn_rva;
    std::uint32_t network_enabled_rva;
    std::uint32_t network_fallback_rva;
    std::uint32_t network_override_rva;
    std::uint32_t network_client_active_rva;
    std::uint32_t client_turn_rva;
    std::uint32_t local_move_history_rva;
    std::uint32_t client_host_turn_rva;
    std::uint32_t select_time_adjustment_rva;
    NativeSite<10> update_turn_store;
    NativeSite<16> predict_turn_reader;
    NativeSite<12> acknowledged_turn_store;
    NativeSite<41> network_state_guard;
    NativeSite<23> client_turn_setter;
    NativeSite<14> move_history_access;
};

// Identifies native selection, presentation, and held-input boundaries.
struct WeaponSwapHooksLayout {
    NativeSite<16> local_select;
    NativeSite<16> packed_sync;
    NativeSite<16> authoritative_select;
    NativeSite<16> packed_select;
    NativeSite<16> base_select;
    NativeSite<16> hud_weapon;
    NativeSite<16> render_selection;
    NativeSite<16> render_channel_selection;
    NativeSite<16> model_selection;
    NativeSite<16> switch_latch;
    NativeSite<5> switch_primary_caller;
    NativeSite<5> switch_secondary_caller;
};

struct WeaponSwapLayout {
    NativeSite<13> joystick_lookup;
    WeaponSwapStateLayout state;
    WeaponSwapHooksLayout hooks;
};

[[nodiscard]] const WeaponSwapLayout& layout_for(TargetLayout target) noexcept;
// Proves state operands and caller identities not covered by an installed hook.
void add_layout_requirements(PatchPlan& plan, const ImageContext& image, const WeaponSwapLayout& layout);
// Resolves the relocated float operand embedded in Weapon::Select.
[[nodiscard]] std::array<std::byte, 16> base_select_preimage(const ImageContext& image,
                                                             const WeaponSwapLayout& layout) noexcept;

} // namespace fusioncutter::patches::weapon_swap_replay
