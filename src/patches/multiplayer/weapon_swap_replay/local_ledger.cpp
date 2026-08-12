#include "local_ledger.hpp"

namespace fusioncutter::patches::weapon_swap_replay {

void LocalSwapLedger::observe_frontiers(const TurnFrontiers& frontiers) noexcept {
    for (auto& epoch : epochs_) {
        if (!epoch.active) {
            continue;
        }
        advance(epoch, frontiers);
        if (turn_distance_greater(epoch.high_predict, epoch.latest_request_turn, kReplayHistoryTurns)) {
            epoch = {};
        }
    }
}

RequestDecision LocalSwapLedger::record_local(void* soldier, int local_player, int channel, int old_index,
                                              int target_index, void* old_weapon, void* target_weapon,
                                              std::int32_t request_turn, const TurnFrontiers& frontiers) noexcept {
    if (!valid_owner(soldier, local_player, channel) || !valid_index(old_index) || !valid_index(target_index) ||
        old_index == target_index || old_weapon == nullptr || target_weapon == nullptr || request_turn < 0) {
        return {};
    }

    auto& epoch = epoch_at(local_player, channel);
    if (!epoch.active || epoch.soldier != soldier) {
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers,
                     RequestKind::New);
    }

    advance(epoch, frontiers);
    const bool historical = turn_after(epoch.high_predict, request_turn);
    for (std::size_t index = 0; index < epoch.step_count; ++index) {
        if (epoch.steps[index].request_turn != request_turn) {
            continue;
        }

        auto& known_old = epoch.node_weapons[static_cast<std::size_t>(old_index)];
        auto& known_target = epoch.node_weapons[static_cast<std::size_t>(target_index)];
        if ((known_old != nullptr && known_old != old_weapon) ||
            (known_target != nullptr && known_target != target_weapon)) {
            if (historical) {
                return abandon(epoch, RequestKind::HistoricalConflict);
            }
            return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers,
                         RequestKind::RestartedDiscontinuity);
        }
        known_old = old_weapon;
        known_target = target_weapon;
        epoch.node_mask = static_cast<std::uint8_t>(epoch.node_mask | (1U << old_index) | (1U << target_index));
        return {RequestKind::Replay, epoch.id};
    }

    if (historical) {
        if (matches_node(epoch, old_index, old_weapon) && matches_node(epoch, target_index, target_weapon)) {
            return {RequestKind::Replay, epoch.id};
        }
        return abandon(epoch, RequestKind::HistoricalConflict);
    }

    if (!turn_after(request_turn, epoch.latest_request_turn)) {
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers,
                     RequestKind::RestartedConflict);
    }

    if (old_index != epoch.final_index || epoch.node_weapons[static_cast<std::size_t>(old_index)] != old_weapon) {
        // A rolled-back transition into the projected target is presentation replay.
        auto& known_old = epoch.node_weapons[static_cast<std::size_t>(old_index)];
        const auto* final_weapon = epoch.node_weapons[static_cast<std::size_t>(epoch.final_index)];
        if (target_index == epoch.final_index && target_weapon == final_weapon &&
            (known_old == nullptr || known_old == old_weapon)) {
            known_old = old_weapon;
            epoch.node_mask = static_cast<std::uint8_t>(epoch.node_mask | (1U << old_index));
            return {RequestKind::Replay, epoch.id};
        }
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers,
                     RequestKind::RestartedDiscontinuity);
    }

    if (epoch.step_count == epoch.steps.size()) {
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers,
                     RequestKind::RestartedOverflow);
    }

    auto& known_target = epoch.node_weapons[static_cast<std::size_t>(target_index)];
    if (known_target != nullptr && known_target != target_weapon) {
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers,
                     RequestKind::RestartedDiscontinuity);
    }

    known_target = target_weapon;
    epoch.steps[epoch.step_count++] = {request_turn};
    epoch.final_index = static_cast<std::uint8_t>(target_index);
    epoch.node_mask = static_cast<std::uint8_t>(epoch.node_mask | (1U << target_index));
    epoch.latest_request_turn = request_turn;
    epoch.latest_sequence = next_nonzero(next_sequence_);
    epoch.settlement_start_update_turn = -1;
    return {RequestKind::New, epoch.id};
}

AuthoritativeDecision LocalSwapLedger::observe_authoritative(void* soldier, int local_player, int channel,
                                                             int current_index, int server_index, void* current_weapon,
                                                             void* server_weapon,
                                                             const TurnFrontiers& frontiers) noexcept {
    if (!valid_owner(soldier, local_player, channel) || !valid_index(current_index) || !valid_index(server_index) ||
        current_weapon == nullptr || server_weapon == nullptr) {
        return {};
    }

    auto& epoch = epoch_at(local_player, channel);
    if (!epoch.active || epoch.soldier != soldier) {
        return {};
    }
    advance(epoch, frontiers);

    const auto current_bit = static_cast<std::uint8_t>(1U << current_index);
    const auto server_bit = static_cast<std::uint8_t>(1U << server_index);
    const bool pointer_invalidated = ((epoch.node_mask & current_bit) != 0 &&
                                      epoch.node_weapons[static_cast<std::size_t>(current_index)] != current_weapon) ||
                                     ((epoch.node_mask & server_bit) != 0 &&
                                      epoch.node_weapons[static_cast<std::size_t>(server_index)] != server_weapon);
    if ((epoch.node_mask & current_bit) == 0 || (epoch.node_mask & server_bit) == 0 || pointer_invalidated) {
        const auto id = epoch.id;
        const auto sequence = epoch.latest_sequence;
        epoch = {};
        return {AuthoritativeKind::Correction, id, sequence, false, pointer_invalidated};
    }

    if (epoch.settlement_start_update_turn < 0 && turn_at_or_after(epoch.high_update, epoch.latest_request_turn) &&
        turn_at_or_after(epoch.high_acknowledged, epoch.latest_request_turn)) {
        epoch.settlement_start_update_turn = epoch.high_update;
    }

    const bool mismatch = current_index != server_index;
    const bool settled = epoch.settlement_start_update_turn >= 0 &&
                         turn_distance_greater(epoch.high_update, epoch.settlement_start_update_turn, kSettlementTurns);
    const auto id = epoch.id;
    const auto sequence = epoch.latest_sequence;
    if (!settled) {
        return {AuthoritativeKind::Pending, id, sequence, mismatch, false};
    }

    if (server_index == epoch.final_index) {
        epoch = {};
        return {AuthoritativeKind::Accepted, id, sequence, mismatch, false};
    }

    epoch = {};
    return {AuthoritativeKind::Rejected, id, sequence, false, false};
}

bool LocalSwapLedger::classify_node_selection(void* soldier, int local_player, int channel, int target_index,
                                              void* target_weapon) const noexcept {
    if (!valid_owner(soldier, local_player, channel) || !valid_index(target_index) || target_weapon == nullptr) {
        return false;
    }
    const auto& epoch = epoch_at(local_player, channel);
    const auto bit = static_cast<std::uint8_t>(1U << target_index);
    return epoch.active && epoch.soldier == soldier && (epoch.node_mask & bit) != 0 &&
           epoch.node_weapons[static_cast<std::size_t>(target_index)] == target_weapon;
}

bool LocalSwapLedger::resolve(void* soldier, int local_player, int channel, int actual_index,
                              EpochSnapshot& snapshot) noexcept {
    snapshot = {};
    if (!valid_owner(soldier, local_player, channel) || !valid_index(actual_index)) {
        return false;
    }
    auto& epoch = epoch_at(local_player, channel);
    if (!epoch.active || epoch.soldier != soldier) {
        return false;
    }
    snapshot = make_snapshot(epoch);
    if (actual_index == epoch.final_index) {
        return false;
    }
    if ((epoch.node_mask & static_cast<std::uint8_t>(1U << actual_index)) != 0) {
        return true;
    }
    epoch = {};
    return false;
}

bool LocalSwapLedger::find(void* soldier, int local_player, int channel, EpochSnapshot& snapshot) const noexcept {
    snapshot = {};
    if (!valid_owner(soldier, local_player, channel)) {
        return false;
    }
    const auto& epoch = epoch_at(local_player, channel);
    if (!epoch.active || epoch.soldier != soldier) {
        return false;
    }
    snapshot = make_snapshot(epoch);
    return true;
}

void LocalSwapLedger::clear_slot(void* soldier, int local_player, int channel) noexcept {
    if (!valid_owner(soldier, local_player, channel)) {
        return;
    }
    auto& epoch = epoch_at(local_player, channel);
    if (epoch.soldier == soldier) {
        epoch = {};
    }
}

void LocalSwapLedger::clear_player(int local_player) noexcept {
    if (local_player < 0 || local_player >= kLocalPlayers) {
        return;
    }
    for (int channel = 0; channel < kWeaponChannels; ++channel) {
        epoch_at(local_player, channel) = {};
    }
}

void LocalSwapLedger::clear() noexcept {
    epochs_ = {};
}

bool LocalSwapLedger::has_active() const noexcept {
    for (const auto& epoch : epochs_) {
        if (epoch.active) {
            return true;
        }
    }
    return false;
}

bool LocalSwapLedger::valid_owner(void* soldier, int local_player, int channel) noexcept {
    return soldier != nullptr && local_player >= 0 && local_player < kLocalPlayers && channel >= 0 &&
           channel < kWeaponChannels;
}

bool LocalSwapLedger::valid_index(int index) noexcept {
    return index >= 0 && index < kWeaponIndices;
}

void LocalSwapLedger::advance(Epoch& epoch, const TurnFrontiers& frontiers) noexcept {
    advance_turn(epoch.high_update, frontiers.update);
    advance_turn(epoch.high_predict, frontiers.predict);
    advance_turn(epoch.high_acknowledged, frontiers.acknowledged);
}

std::uint32_t LocalSwapLedger::next_nonzero(std::uint32_t& value) noexcept {
    if (++value == 0) {
        ++value;
    }
    return value;
}

bool LocalSwapLedger::matches_node(const Epoch& epoch, int index, void* weapon) noexcept {
    const auto bit = static_cast<std::uint8_t>(1U << index);
    return (epoch.node_mask & bit) != 0 && epoch.node_weapons[static_cast<std::size_t>(index)] == weapon;
}

RequestDecision LocalSwapLedger::abandon(Epoch& epoch, RequestKind kind) noexcept {
    const auto id = epoch.id;
    epoch = {};
    return {kind, id};
}

RequestDecision LocalSwapLedger::start(Epoch& epoch, void* soldier, int old_index, int target_index, void* old_weapon,
                                       void* target_weapon, std::int32_t request_turn, const TurnFrontiers& frontiers,
                                       RequestKind kind) noexcept {
    epoch = {};
    epoch.active = true;
    epoch.soldier = soldier;
    epoch.final_index = static_cast<std::uint8_t>(target_index);
    epoch.node_mask = static_cast<std::uint8_t>((1U << old_index) | (1U << target_index));
    epoch.node_weapons[static_cast<std::size_t>(old_index)] = old_weapon;
    epoch.node_weapons[static_cast<std::size_t>(target_index)] = target_weapon;
    epoch.latest_request_turn = request_turn;
    epoch.id = next_nonzero(next_epoch_);
    epoch.latest_sequence = next_nonzero(next_sequence_);
    epoch.steps[0] = {request_turn};
    epoch.step_count = 1;
    advance(epoch, frontiers);
    return {kind, epoch.id};
}

EpochSnapshot LocalSwapLedger::make_snapshot(const Epoch& epoch) noexcept {
    return {epoch.soldier, epoch.node_weapons,   epoch.node_mask, epoch.final_index, epoch.latest_request_turn,
            epoch.id,      epoch.latest_sequence};
}

std::size_t LocalSwapLedger::slot(int local_player, int channel) noexcept {
    return static_cast<std::size_t>(local_player) * kWeaponChannels + static_cast<std::size_t>(channel);
}

LocalSwapLedger::Epoch& LocalSwapLedger::epoch_at(int local_player, int channel) noexcept {
    return epochs_[slot(local_player, channel)];
}

const LocalSwapLedger::Epoch& LocalSwapLedger::epoch_at(int local_player, int channel) const noexcept {
    return epochs_[slot(local_player, channel)];
}

} // namespace fusioncutter::patches::weapon_swap_replay
