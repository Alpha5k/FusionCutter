#include "packed_ledger.hpp"

namespace fusioncutter::patches::weapon_swap_replay {

void PackedSwapLedger::observe_frontiers(const TurnFrontiers& frontiers) noexcept {
    for (auto& lane : lanes_) {
        if (!lane.active) {
            continue;
        }
        advance(lane, frontiers);
        if (turn_distance_greater(lane.high_predict, lane.latest_request_turn, kReplayHistoryTurns)) {
            lane = {};
        }
    }
}

bool PackedSwapLedger::track_projected(void* soldier, int local_player, int channel, int index, void* weapon,
                                       std::int32_t request_turn, std::uint32_t epoch, std::uint32_t sequence,
                                       const TurnFrontiers& frontiers) noexcept {
    if (!valid_owner(soldier, local_player) || !valid_key(channel, index) || weapon == nullptr || request_turn < 0 ||
        epoch == 0 || sequence == 0) {
        return false;
    }

    auto& lane = lanes_[static_cast<std::size_t>(local_player)];
    const int projected = key(channel, index);
    if (!lane.active || lane.soldier != soldier || lane.epoch != epoch) {
        if (lane.active && lane.soldier == soldier && turn_after(lane.latest_request_turn, request_turn)) {
            return false;
        }
        start(lane, soldier, projected, weapon, request_turn, epoch, sequence, frontiers);
        return true;
    }

    advance(lane, frontiers);
    if (turn_after(lane.latest_request_turn, request_turn) ||
        (lane.latest_request_turn == request_turn && lane.sequence != sequence)) {
        return false;
    }

    const auto bit = static_cast<std::uint16_t>(1U << projected);
    auto& known = lane.node_weapons[static_cast<std::size_t>(projected)];
    if ((lane.node_mask & bit) != 0 && known != weapon) {
        start(lane, soldier, projected, weapon, request_turn, epoch, sequence, frontiers);
        return true;
    }

    known = weapon;
    lane.node_mask = static_cast<std::uint16_t>(lane.node_mask | bit);
    lane.projected_key = static_cast<std::uint8_t>(projected);
    lane.latest_request_turn = request_turn;
    lane.sequence = sequence;
    lane.settled = false;
    return true;
}

PackedDecision PackedSwapLedger::observe_transition(void* soldier, int local_player, int old_channel, int old_index,
                                                    void* old_weapon, int target_channel, int target_index,
                                                    void* target_weapon, const TurnFrontiers& frontiers) noexcept {
    if (!valid_owner(soldier, local_player) || !valid_key(old_channel, old_index) ||
        !valid_key(target_channel, target_index) || old_weapon == nullptr || target_weapon == nullptr) {
        return {};
    }

    auto& lane = lanes_[static_cast<std::size_t>(local_player)];
    if (!lane.active || lane.soldier != soldier) {
        return {};
    }
    advance(lane, frontiers);

    const int old_key = key(old_channel, old_index);
    const int target_key = key(target_channel, target_index);
    const auto old_bit = static_cast<std::uint16_t>(1U << old_key);
    const auto target_bit = static_cast<std::uint16_t>(1U << target_key);
    const bool old_recorded = (lane.node_mask & old_bit) != 0;
    const bool target_recorded = (lane.node_mask & target_bit) != 0;
    const bool pointer_invalidated =
        (old_recorded && lane.node_weapons[static_cast<std::size_t>(old_key)] != old_weapon) ||
        (target_recorded && lane.node_weapons[static_cast<std::size_t>(target_key)] != target_weapon);
    const bool new_endpoint = old_recorded != target_recorded;
    if (pointer_invalidated || (!old_recorded && !target_recorded) || (lane.settled && new_endpoint)) {
        lane = {};
        return {false, pointer_invalidated};
    }

    lane.node_weapons[static_cast<std::size_t>(old_key)] = old_weapon;
    lane.node_weapons[static_cast<std::size_t>(target_key)] = target_weapon;
    lane.node_mask = static_cast<std::uint16_t>(lane.node_mask | old_bit | target_bit);
    return {true, false};
}

PackedDecision PackedSwapLedger::observe_sync(void* soldier, int local_player, int channel, int index, void* weapon,
                                              const TurnFrontiers& frontiers) noexcept {
    if (!valid_owner(soldier, local_player) || !valid_key(channel, index) || weapon == nullptr) {
        return {};
    }

    auto& lane = lanes_[static_cast<std::size_t>(local_player)];
    if (!lane.active || lane.soldier != soldier) {
        return {};
    }
    advance(lane, frontiers);

    const int observed_key = key(channel, index);
    const auto bit = static_cast<std::uint16_t>(1U << observed_key);
    if ((lane.node_mask & bit) != 0 && lane.node_weapons[static_cast<std::size_t>(observed_key)] == weapon) {
        return {true, false};
    }

    const bool pointer_invalidated = (lane.node_mask & bit) != 0;
    lane = {};
    return {false, pointer_invalidated};
}

void PackedSwapLedger::observe_channel_result(int local_player, const AuthoritativeDecision& decision) noexcept {
    if (local_player < 0 || local_player >= kLocalPlayers) {
        return;
    }
    auto& lane = lanes_[static_cast<std::size_t>(local_player)];
    if (!lane.active || lane.epoch != decision.epoch || lane.sequence != decision.sequence) {
        return;
    }
    if (decision.kind == AuthoritativeKind::Accepted) {
        lane.settled = true;
    } else if (decision.kind == AuthoritativeKind::Rejected || decision.kind == AuthoritativeKind::Correction) {
        lane = {};
    }
}

bool PackedSwapLedger::resolve(void* soldier, int local_player, int actual_channel, int actual_index,
                               void* actual_weapon, PackedSnapshot& snapshot) noexcept {
    snapshot = {};
    if (!valid_owner(soldier, local_player) || !valid_key(actual_channel, actual_index) || actual_weapon == nullptr) {
        return false;
    }
    auto& lane = lanes_[static_cast<std::size_t>(local_player)];
    if (!lane.active || lane.soldier != soldier) {
        return false;
    }
    snapshot = make_snapshot(lane);
    const int actual_key = key(actual_channel, actual_index);
    const auto actual_bit = static_cast<std::uint16_t>(1U << actual_key);
    if ((lane.node_mask & actual_bit) == 0 ||
        lane.node_weapons[static_cast<std::size_t>(actual_key)] != actual_weapon) {
        lane = {};
        return false;
    }
    return actual_key != lane.projected_key;
}

bool PackedSwapLedger::has_lane(void* soldier, int local_player) const noexcept {
    if (!valid_owner(soldier, local_player)) {
        return false;
    }
    const auto& lane = lanes_[static_cast<std::size_t>(local_player)];
    return lane.active && lane.soldier == soldier;
}

bool PackedSwapLedger::find(void* soldier, int local_player, PackedSnapshot& snapshot) const noexcept {
    snapshot = {};
    if (!valid_owner(soldier, local_player)) {
        return false;
    }
    const auto& lane = lanes_[static_cast<std::size_t>(local_player)];
    if (!lane.active || lane.soldier != soldier) {
        return false;
    }
    snapshot = make_snapshot(lane);
    return true;
}

void PackedSwapLedger::clear_player(int local_player) noexcept {
    if (local_player >= 0 && local_player < kLocalPlayers) {
        lanes_[static_cast<std::size_t>(local_player)] = {};
    }
}

void PackedSwapLedger::clear_epoch(void* soldier, int local_player, std::uint32_t epoch) noexcept {
    if (!valid_owner(soldier, local_player) || epoch == 0) {
        return;
    }
    auto& lane = lanes_[static_cast<std::size_t>(local_player)];
    if (lane.active && lane.soldier == soldier && lane.epoch == epoch) {
        lane = {};
    }
}

void PackedSwapLedger::clear() noexcept {
    lanes_ = {};
}

bool PackedSwapLedger::has_active() const noexcept {
    for (const auto& lane : lanes_) {
        if (lane.active) {
            return true;
        }
    }
    return false;
}

bool PackedSwapLedger::valid_owner(void* soldier, int local_player) noexcept {
    return soldier != nullptr && local_player >= 0 && local_player < kLocalPlayers;
}

bool PackedSwapLedger::valid_key(int channel, int index) noexcept {
    return channel >= 0 && channel < kWeaponChannels && index >= 0 && index < kWeaponIndices;
}

void PackedSwapLedger::advance(Lane& lane, const TurnFrontiers& frontiers) noexcept {
    advance_turn(lane.high_predict, frontiers.predict);
}

void PackedSwapLedger::start(Lane& lane, void* soldier, int projected_key, void* weapon, std::int32_t request_turn,
                             std::uint32_t epoch, std::uint32_t sequence, const TurnFrontiers& frontiers) noexcept {
    lane = {};
    lane.active = true;
    lane.soldier = soldier;
    lane.projected_key = static_cast<std::uint8_t>(projected_key);
    lane.node_mask = static_cast<std::uint16_t>(1U << projected_key);
    lane.node_weapons[static_cast<std::size_t>(projected_key)] = weapon;
    lane.latest_request_turn = request_turn;
    lane.epoch = epoch;
    lane.sequence = sequence;
    advance(lane, frontiers);
}

PackedSnapshot PackedSwapLedger::make_snapshot(const Lane& lane) noexcept {
    return {lane.soldier, lane.node_weapons, lane.node_mask, lane.projected_key};
}

} // namespace fusioncutter::patches::weapon_swap_replay
