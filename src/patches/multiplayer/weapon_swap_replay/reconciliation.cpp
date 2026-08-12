#include "weapon_swap.hpp"

#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::weapon_swap_replay {
namespace {

constexpr std::size_t kSelectedIndexOffset = 0x740;
constexpr std::size_t kPackedSelectionOffset = 0x742;
constexpr std::size_t kLocalSelectChannelOffset = 0x20;
constexpr std::size_t kLocalSelectOldIndexOffset = 0x28;
constexpr std::size_t kPackedSyncTargetWeaponOffset = 0x38;
constexpr std::size_t kAuthoritativeCurrentIndexOffset = 0x1C;

} // namespace

bool WeaponSwapReplayFix::track_matching_projection(void* soldier, int local_player, int channel, int index,
                                                    void* weapon, const TurnFrontiers& frontiers) noexcept {
    if (index < 0 || index >= kWeaponIndices) {
        return false;
    }
    EpochSnapshot projection;
    if (!ledger_.find(soldier, local_player, channel, projection) || projection.final_index != index ||
        projection.node_weapons[static_cast<std::size_t>(index)] != weapon) {
        return false;
    }
    return packed_ledger_.track_projected(soldier, local_player, channel, index, weapon, projection.latest_request_turn,
                                          projection.epoch, projection.sequence, frontiers);
}

void WeaponSwapReplayFix::handle_local_select(void* adjusted_soldier, int channel, int old_index,
                                              int target_index) noexcept {
    select_intent_.clear();
    observe_lifecycle();
    auto* soldier = soldier_from_controllable(adjusted_soldier);
    const int local_player = local_player_index(soldier);
    if (!network_prediction_active() || local_player < 0 || local_player >= kLocalPlayers || channel < 0 ||
        channel >= kWeaponChannels) {
        return;
    }

    auto& tracked = local_soldiers_[static_cast<std::size_t>(local_player)];
    if (tracked != nullptr && tracked != soldier) {
        ledger_.clear_player(local_player);
        packed_ledger_.clear_player(local_player);
    }
    tracked = soldier;

    auto* old_weapon = weapon_at(soldier, old_index);
    auto* target_weapon = weapon_at(soldier, target_index);
    if (old_weapon == nullptr || target_weapon == nullptr) {
        return;
    }

    const auto current_frontiers = frontiers();
    const auto decision = ledger_.record_local(soldier, local_player, channel, old_index, target_index, old_weapon,
                                               target_weapon, current_frontiers.predict, current_frontiers);
    if (!decision.valid()) {
        return;
    }
    if (decision.failed_open()) {
        packed_ledger_.clear_epoch(soldier, local_player, decision.epoch);
        update_presentation_activity();
        return;
    }

    const auto packed = read_native_field<std::uint8_t>(soldier, kPackedSelectionOffset);
    EpochSnapshot projected;
    if (packed_weapon_channel(packed) == channel && ledger_.find(soldier, local_player, channel, projected)) {
        static_cast<void>(packed_ledger_.track_projected(
            soldier, local_player, channel, projected.final_index,
            projected.node_weapons[static_cast<std::size_t>(projected.final_index)], projected.latest_request_turn,
            projected.epoch, projected.sequence, current_frontiers));
    }

    select_intent_.arm(target_weapon, channel, decision.replay());
    update_presentation_activity();
}

void WeaponSwapReplayFix::handle_packed_sync(void* adjusted_soldier, void* target_weapon) noexcept {
    select_intent_.clear();
    observe_lifecycle();
    if (adjusted_soldier == nullptr || target_weapon == nullptr || !network_prediction_active()) {
        return;
    }

    auto* soldier = soldier_from_controllable(adjusted_soldier);
    const int local_player = tracked_local_player(soldier);
    if (local_player < 0) {
        return;
    }

    const auto packed = read_native_field<std::uint8_t>(soldier, kPackedSelectionOffset);
    const int target_index = packed_weapon_index(packed);
    const int channel = packed_weapon_channel(packed);
    if (channel < 0 || channel >= kWeaponChannels || weapon_at(soldier, target_index) != target_weapon) {
        return;
    }

    const auto current_frontiers = frontiers();
    static_cast<void>(
        track_matching_projection(soldier, local_player, channel, target_index, target_weapon, current_frontiers));

    const auto packed_decision =
        packed_ledger_.observe_sync(soldier, local_player, channel, target_index, target_weapon, current_frontiers);
    const bool channel_duplicate =
        ledger_.classify_node_selection(soldier, local_player, channel, target_index, target_weapon);
    select_intent_.arm(target_weapon, channel, packed_decision.mute_select || channel_duplicate);
}

void WeaponSwapReplayFix::handle_authoritative_select(void* soldier, int channel, int current_index,
                                                      int server_index) noexcept {
    select_intent_.clear();
    observe_lifecycle();
    if (!network_prediction_active()) {
        return;
    }

    const int local_player = tracked_local_player(soldier);
    if (local_player < 0 || channel < 0 || channel >= kWeaponChannels || current_index < 0 ||
        current_index >= kWeaponIndices || server_index < 0 || server_index >= kWeaponIndices) {
        return;
    }

    auto* current_weapon = weapon_at(soldier, current_index);
    auto* target_weapon = weapon_at(soldier, server_index);
    const auto decision = ledger_.observe_authoritative(soldier, local_player, channel, current_index, server_index,
                                                        current_weapon, target_weapon, frontiers());
    packed_ledger_.observe_channel_result(local_player, decision);
    if (current_index != server_index && target_weapon != nullptr) {
        select_intent_.arm(target_weapon, channel, decision.mute_select);
    }
    update_presentation_activity();
}

void WeaponSwapReplayFix::handle_packed_select(void* soldier, int old_channel, int old_index,
                                               std::uint8_t server_packed) noexcept {
    select_intent_.clear();
    observe_lifecycle();
    if (!network_prediction_active()) {
        return;
    }

    const int server_index = packed_weapon_index(server_packed);
    const int server_channel = packed_weapon_channel(server_packed);
    const int local_player = tracked_local_player(soldier);
    if (local_player < 0 || old_channel < 0 || old_channel >= kWeaponChannels || server_channel < 0 ||
        server_channel >= kWeaponChannels || old_index < 0 || old_index >= kWeaponIndices || server_index < 0 ||
        server_index >= kWeaponIndices || (old_channel == server_channel && old_index == server_index)) {
        return;
    }

    auto* old_weapon = weapon_at(soldier, old_index);
    auto* target_weapon = weapon_at(soldier, server_index);
    if (old_weapon == nullptr || target_weapon == nullptr) {
        return;
    }

    const auto current_frontiers = frontiers();
    if (!packed_ledger_.has_lane(soldier, local_player)) {
        if (!track_matching_projection(soldier, local_player, old_channel, old_index, old_weapon, current_frontiers)) {
            static_cast<void>(track_matching_projection(soldier, local_player, server_channel, server_index,
                                                        target_weapon, current_frontiers));
        }
    }

    const auto packed_decision =
        packed_ledger_.observe_transition(soldier, local_player, old_channel, old_index, old_weapon, server_channel,
                                          server_index, target_weapon, current_frontiers);
    const bool channel_duplicate =
        ledger_.classify_node_selection(soldier, local_player, server_channel, server_index, target_weapon);
    select_intent_.arm(target_weapon, server_channel, packed_decision.mute_select || channel_duplicate);
}

void WeaponSwapReplayFix::observe_local_select(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.edi == 0 || context.esp == 0) {
        return;
    }
    const auto* stack = reinterpret_cast<const void*>(context.esp);
    patch->handle_local_select(
        reinterpret_cast<void*>(context.edi), read_native_field<int>(stack, kLocalSelectChannelOffset),
        read_native_field<int>(stack, kLocalSelectOldIndexOffset), static_cast<int>(context.eax));
}

void WeaponSwapReplayFix::observe_packed_sync(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.edi == 0 || context.esp == 0) {
        return;
    }
    patch->handle_packed_sync(
        reinterpret_cast<void*>(context.edi),
        read_native_field<void*>(reinterpret_cast<const void*>(context.esp), kPackedSyncTargetWeaponOffset));
}

void WeaponSwapReplayFix::observe_authoritative_select(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.edi == 0 || context.edx == 0 || context.esp == 0) {
        return;
    }

    const auto selected_offset = static_cast<std::intptr_t>(context.edx) - static_cast<std::intptr_t>(context.edi) -
                                 static_cast<std::intptr_t>(kSelectedIndexOffset);
    patch->handle_authoritative_select(
        reinterpret_cast<void*>(context.edi), static_cast<int>(selected_offset),
        read_native_field<std::uint8_t>(reinterpret_cast<const void*>(context.esp), kAuthoritativeCurrentIndexOffset),
        read_native_field<std::int8_t>(reinterpret_cast<const void*>(context.edx)));
}

void WeaponSwapReplayFix::observe_packed_select(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.edi == 0) {
        return;
    }
    auto* soldier = reinterpret_cast<void*>(context.edi);
    patch->handle_packed_select(soldier, static_cast<int>(context.ecx), static_cast<int>(context.edx),
                                read_native_field<std::uint8_t>(soldier, kPackedSelectionOffset));
}

} // namespace fusioncutter::patches::weapon_swap_replay
