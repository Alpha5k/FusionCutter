#include "weapon_swap.hpp"

#include <cstddef>
#include <utility>

namespace fusioncutter::patches::weapon_swap_replay {
namespace {

constexpr std::size_t kControllableOffset = 0x240;
constexpr std::size_t kPlayerHandleOffset = 0x314;
constexpr std::size_t kWeaponArrayOffset = 0x720;

} // namespace

WeaponSwapReplayFix::WeaponSwapReplayFix(const TargetContext& target) noexcept
    : layout_(layout_for(target.layout)), image_(target.image),
      get_joystick_index_(image_.function_at_rva<GetJoystickIndex>(layout_.joystick_lookup.rva)),
      update_turn_(image_.read_at_rva<volatile std::int32_t>(layout_.state.update_turn_rva)),
      predict_turn_(image_.read_at_rva<volatile std::int32_t>(layout_.state.predict_turn_rva)),
      acknowledged_turn_(image_.read_at_rva<volatile std::int32_t>(layout_.state.acknowledged_turn_rva)),
      network_enabled_(image_.read_at_rva<volatile std::uint8_t>(layout_.state.network_enabled_rva)),
      network_client_active_(image_.read_at_rva<volatile std::uint8_t>(layout_.state.network_client_active_rva)),
      client_turn_(image_.read_at_rva<volatile std::int32_t>(layout_.state.client_turn_rva)),
      local_move_history_(image_.read_at_rva<std::byte>(layout_.state.local_move_history_rva)),
      select_time_adjustment_(image_.read_at_rva<float>(layout_.state.select_time_adjustment_rva)),
      switch_primary_return_(image_.address_at_rva(layout_.hooks.switch_primary_caller.rva +
                                                   layout_.hooks.switch_primary_caller.expected.size())),
      switch_secondary_return_(image_.address_at_rva(layout_.hooks.switch_secondary_caller.rva +
                                                     layout_.hooks.switch_secondary_caller.expected.size())) {}

void WeaponSwapReplayFix::build_plan(PatchPlan& plan) {
    add_layout_requirements(plan, image_, layout_);
    plan.mid_hook("Observe local weapon selection", layout_.hooks.local_select.rva,
                  layout_.hooks.local_select.pattern(), &WeaponSwapReplayFix::observe_local_select);
    plan.mid_hook("Observe packed weapon synchronization", layout_.hooks.packed_sync.rva,
                  layout_.hooks.packed_sync.pattern(), &WeaponSwapReplayFix::observe_packed_sync);
    plan.mid_hook("Observe authoritative weapon selection", layout_.hooks.authoritative_select.rva,
                  layout_.hooks.authoritative_select.pattern(), &WeaponSwapReplayFix::observe_authoritative_select);
    plan.mid_hook("Observe authoritative packed selection", layout_.hooks.packed_select.rva,
                  layout_.hooks.packed_select.pattern(), &WeaponSwapReplayFix::observe_packed_select);
    const auto base_select = base_select_preimage(image_, layout_);
    plan.mid_hook("Suppress duplicate weapon presentation", layout_.hooks.base_select.rva,
                  BytePattern::exact(base_select), &WeaponSwapReplayFix::suppress_duplicate_select);
    plan.mid_hook("Project weapon selection to the HUD", layout_.hooks.hud_weapon.rva,
                  layout_.hooks.hud_weapon.pattern(), &WeaponSwapReplayFix::project_hud_weapon);
    plan.mid_hook("Project weapon selection during render lookup", layout_.hooks.render_selection.rva,
                  layout_.hooks.render_selection.pattern(), &WeaponSwapReplayFix::project_render_selection);
    plan.mid_hook("Project weapon channel during render lookup", layout_.hooks.render_channel_selection.rva,
                  layout_.hooks.render_channel_selection.pattern(),
                  &WeaponSwapReplayFix::project_render_channel_selection);
    plan.mid_hook("Project weapon selection to the model", layout_.hooks.model_selection.rva,
                  layout_.hooks.model_selection.pattern(), &WeaponSwapReplayFix::project_model_selection);
    plan.mid_hook("Ignore replayed held weapon-switch input", layout_.hooks.switch_latch.rva,
                  layout_.hooks.switch_latch.pattern(), &WeaponSwapReplayFix::filter_held_switch);
}

void WeaponSwapReplayFix::enable_runtime() noexcept {
    clear_tracking();
    active_.publish(*this);
}

void WeaponSwapReplayFix::disable_runtime() noexcept {
    active_.clear(*this);
    clear_tracking();
}

int WeaponSwapReplayFix::local_player_index(void* soldier) const noexcept {
    if (soldier == nullptr) {
        return -1;
    }
    return get_joystick_index_(read_native_field<int>(soldier, kPlayerHandleOffset));
}

int WeaponSwapReplayFix::tracked_local_player(void* soldier) const noexcept {
    for (int index = 0; index < kLocalPlayers; ++index) {
        if (local_soldiers_[static_cast<std::size_t>(index)] == soldier) {
            return index;
        }
    }
    return -1;
}

bool WeaponSwapReplayFix::network_prediction_active() const noexcept {
    return *network_enabled_ != 0 && *network_client_active_ != 0 && *update_turn_ > 0 && *predict_turn_ > 0 &&
           *acknowledged_turn_ >= 0;
}

void WeaponSwapReplayFix::observe_lifecycle() noexcept {
    if (*network_enabled_ == 0 || *network_client_active_ == 0) {
        if (lifecycle_.active() || ledger_.has_active() || packed_ledger_.has_active()) {
            clear_tracking();
        }
        return;
    }

    const auto observed = frontiers();
    if (lifecycle_.observe(observed)) {
        clear_prediction_state();
    }
    if (lifecycle_.active()) {
        ledger_.observe_frontiers(observed);
        packed_ledger_.observe_frontiers(observed);
        update_presentation_activity();
    }
}

void WeaponSwapReplayFix::update_presentation_activity() noexcept {
    presentation_active_ = ledger_.has_active() || packed_ledger_.has_active();
}

void WeaponSwapReplayFix::clear_prediction_state() noexcept {
    ledger_.clear();
    packed_ledger_.clear();
    local_soldiers_ = {};
    presentation_active_ = false;
    select_intent_.clear();
}

void WeaponSwapReplayFix::clear_tracking() noexcept {
    clear_prediction_state();
    lifecycle_.clear();
}

TurnFrontiers WeaponSwapReplayFix::frontiers() const noexcept {
    return {*update_turn_, *predict_turn_, *acknowledged_turn_};
}

bool WeaponSwapReplayFix::validate_snapshot(const EpochSnapshot& snapshot) const noexcept {
    if (snapshot.soldier == nullptr || snapshot.node_mask == 0) {
        return false;
    }
    for (int index = 0; index < kWeaponIndices; ++index) {
        const auto bit = static_cast<std::uint8_t>(1U << index);
        if ((snapshot.node_mask & bit) != 0 &&
            weapon_at(snapshot.soldier, index) != snapshot.node_weapons[static_cast<std::size_t>(index)]) {
            return false;
        }
    }
    return true;
}

bool WeaponSwapReplayFix::validate_packed_snapshot(const PackedSnapshot& snapshot) const noexcept {
    if (snapshot.soldier == nullptr || snapshot.node_mask == 0 || snapshot.projected_key >= PackedSwapLedger::kKeys) {
        return false;
    }
    const auto projected_bit = static_cast<std::uint16_t>(1U << snapshot.projected_key);
    if ((snapshot.node_mask & projected_bit) == 0 ||
        snapshot.node_weapons[static_cast<std::size_t>(snapshot.projected_key)] == nullptr) {
        return false;
    }
    for (int key = 0; key < PackedSwapLedger::kKeys; ++key) {
        const auto bit = static_cast<std::uint16_t>(1U << key);
        if ((snapshot.node_mask & bit) != 0 &&
            weapon_at(snapshot.soldier, key % kWeaponIndices) != snapshot.node_weapons[static_cast<std::size_t>(key)]) {
            return false;
        }
    }
    return true;
}

void* WeaponSwapReplayFix::soldier_from_controllable(void* controllable) noexcept {
    return controllable == nullptr ? nullptr : static_cast<std::byte*>(controllable) - kControllableOffset;
}

void* WeaponSwapReplayFix::weapon_at(void* soldier, int index) noexcept {
    if (soldier == nullptr || index < 0 || index >= kWeaponIndices) {
        return nullptr;
    }
    return read_native_field<void*>(soldier, kWeaponArrayOffset + static_cast<std::size_t>(index) * sizeof(void*));
}

int WeaponSwapReplayFix::weapon_index(void* soldier, void* weapon) noexcept {
    if (soldier == nullptr || weapon == nullptr) {
        return -1;
    }
    for (int index = 0; index < kWeaponIndices; ++index) {
        if (weapon_at(soldier, index) == weapon) {
            return index;
        }
    }
    return -1;
}

} // namespace fusioncutter::patches::weapon_swap_replay
