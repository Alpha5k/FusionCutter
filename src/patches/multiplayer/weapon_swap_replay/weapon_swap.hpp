#pragma once

#include "layout.hpp"
#include "packed_ledger.hpp"

#include <FusionCutter/patch.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::weapon_swap_replay {

// Reconciles local weapon-swap prediction without replacing native selection transactions.
class WeaponSwapReplayFix final : public RuntimePatch {
  public:
    explicit WeaponSwapReplayFix(const TargetContext& target) noexcept;

    // Validates every reconciliation boundary and installs the observation and presentation hooks.
    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using GetJoystickIndex = int(__cdecl*)(int player_handle) noexcept;

    // Classify predicted and authoritative selection paths into one causal request history.
    void handle_local_select(void* adjusted_soldier, int channel, int old_index, int target_index) noexcept;
    void handle_packed_sync(void* adjusted_soldier, void* target_weapon) noexcept;
    void handle_authoritative_select(void* soldier, int channel, int current_index, int server_index) noexcept;
    void handle_packed_select(void* soldier, int old_channel, int old_index, std::uint8_t server_packed) noexcept;
    [[nodiscard]] bool track_matching_projection(void* soldier, int local_player, int channel, int index, void* weapon,
                                                 const TurnFrontiers& frontiers) noexcept;

    // Present the predicted endpoint while native reconciliation still owns gameplay state.
    [[nodiscard]] void* resolve_hud_weapon(void* controllable, int channel, void* actual_weapon) noexcept;
    [[nodiscard]] std::uint8_t resolve_packed_selection(void* soldier, std::uint8_t packed) noexcept;
    [[nodiscard]] bool suppress_held_switch(void* controllable, int channel, std::uint32_t direction) const noexcept;

    // Keep request ownership synchronized with network and local-player lifecycle changes.
    [[nodiscard]] int local_player_index(void* soldier) const noexcept;
    [[nodiscard]] int tracked_local_player(void* soldier) const noexcept;
    [[nodiscard]] bool is_tracked_local_soldier(void* soldier, int local_player) const noexcept;
    [[nodiscard]] bool network_prediction_active() const noexcept;
    void observe_lifecycle() noexcept;
    void update_presentation_activity() noexcept;
    void clear_prediction_state() noexcept;
    void clear_tracking() noexcept;
    [[nodiscard]] TurnFrontiers frontiers() const noexcept;
    [[nodiscard]] bool validate_snapshot(const EpochSnapshot& snapshot) const noexcept;
    [[nodiscard]] bool validate_packed_snapshot(const PackedSnapshot& snapshot) const noexcept;
    [[nodiscard]] static void* soldier_from_controllable(void* controllable) noexcept;
    [[nodiscard]] static void* weapon_at(void* soldier, int index) noexcept;
    [[nodiscard]] static int weapon_index(void* soldier, void* weapon) noexcept;
    [[nodiscard]] bool held_switch_replay(int local_player, int channel, std::uint32_t trigger,
                                          std::uint32_t direction) const noexcept;

    // Adapt each validated native register boundary to the typed patch behavior above.
    static void observe_local_select(MidHookContext& context) noexcept;
    static void observe_packed_sync(MidHookContext& context) noexcept;
    static void observe_authoritative_select(MidHookContext& context) noexcept;
    static void observe_packed_select(MidHookContext& context) noexcept;
    static void suppress_duplicate_select(MidHookContext& context) noexcept;
    static void project_hud_weapon(MidHookContext& context) noexcept;
    static void project_render_selection(MidHookContext& context) noexcept;
    static void project_render_channel_selection(MidHookContext& context) noexcept;
    static void project_model_selection(MidHookContext& context) noexcept;
    static void filter_held_switch(MidHookContext& context) noexcept;

    const WeaponSwapLayout& layout_;
    ImageContext image_;
    GetJoystickIndex get_joystick_index_{};
    const volatile std::int32_t* update_turn_{};
    const volatile std::int32_t* predict_turn_{};
    const volatile std::int32_t* acknowledged_turn_{};
    const volatile std::uint8_t* network_enabled_{};
    const volatile std::uint8_t* network_client_active_{};
    const volatile std::int32_t* client_turn_{};
    const std::byte* local_move_history_{};
    std::uintptr_t switch_primary_return_{};
    std::uintptr_t switch_secondary_return_{};
    LocalSwapLedger ledger_;
    PackedSwapLedger packed_ledger_;
    std::array<void*, kLocalPlayers> local_soldiers_{};
    NetworkLifecycle lifecycle_;
    bool presentation_active_{};

    inline static PatchInstanceSlot<WeaponSwapReplayFix> active_;
    inline static thread_local SelectIntent select_intent_;
};

} // namespace fusioncutter::patches::weapon_swap_replay
