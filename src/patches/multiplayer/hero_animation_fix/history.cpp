#include "history.hpp"

#include <algorithm>

namespace fusioncutter::patches::hero_animation_fix {
namespace {

[[nodiscard]] constexpr bool valid_state(int state) noexcept {
    return state >= 0 && state < 32;
}

[[nodiscard]] constexpr std::uint32_t state_bit(int state) noexcept {
    return std::uint32_t{1} << static_cast<std::uint32_t>(state);
}

[[nodiscard]] constexpr bool turn_after(std::int32_t turn, std::int32_t reference) noexcept {
    return turn >= 0 && reference >= 0 &&
           static_cast<std::int32_t>(static_cast<std::uint32_t>(turn) - static_cast<std::uint32_t>(reference)) > 0;
}

} // namespace

bool HeroHistory::observe_prediction(const HeroIdentity& identity, int base_state, int target_state,
                                     std::int32_t prediction_turn) noexcept {
    if (!identity.valid() || !valid_state(base_state) || !valid_state(target_state) || base_state == target_state) {
        return false;
    }
    static_cast<void>(bind_receipt(identity));
    return identity.is_local() ? observe_local_prediction(identity, base_state, target_state, prediction_turn)
                               : observe_remote_prediction(identity, base_state, target_state);
}

void HeroHistory::begin_prediction(const HeroIdentity& identity) noexcept {
    if (!identity.valid()) {
        return;
    }
    static_cast<void>(bind_receipt(identity));
    if (identity.is_local()) {
        auto& epoch = local_[identity.local_player];
        if (epoch.identity.valid() && epoch.identity != identity) {
            epoch = {};
        }
    } else {
        static_cast<void>(bind_remote(identity));
    }
}

bool HeroHistory::observe_local_prediction(const HeroIdentity& identity, int base_state, int target_state,
                                           std::int32_t prediction_turn) noexcept {
    if (prediction_turn <= 0) {
        return false;
    }

    auto& epoch = local_[identity.local_player];
    const auto base_known = epoch.identity.valid() && (epoch.seen_states & state_bit(base_state)) != 0;
    const auto starts_action = base_state == 0 && target_state != 0;
    if (epoch.identity != identity || starts_action || epoch.predicted_state != base_state || !base_known) {
        epoch = {
            .identity = identity,
            .seen_states = state_bit(base_state) | state_bit(target_state),
            .start_turn = prediction_turn,
            .finish_turn = target_state == 0 ? prediction_turn : -1,
            .predicted_state = static_cast<std::int8_t>(target_state),
        };
        return true;
    }

    epoch.seen_states |= state_bit(target_state);
    epoch.predicted_state = static_cast<std::int8_t>(target_state);
    if (target_state == 0) {
        epoch.finish_turn = prediction_turn;
    }
    return true;
}

bool HeroHistory::observe_remote_prediction(const HeroIdentity& identity, int base_state, int target_state) noexcept {
    auto& slot = bind_remote(identity);
    if (!slot.authority_seen) {
        seed_remote(slot, base_state);
    } else if (expected_remote_state(slot) != base_state) {
        seed_remote(slot, base_state);
    }

    if (slot.count == slot.pending.size()) {
        seed_remote(slot, base_state);
        return false;
    }
    slot.pending[slot.count++] = static_cast<std::int8_t>(target_state);
    return true;
}

AuthorityAction HeroHistory::classify_authority(const HeroIdentity& identity, int current_state, int incoming_state,
                                                std::int32_t acknowledged_turn) noexcept {
    if (!identity.valid() || !valid_state(current_state) || !valid_state(incoming_state)) {
        return AuthorityAction::Apply;
    }
    static_cast<void>(bind_receipt(identity));
    return identity.is_local() ? classify_local_authority(identity, current_state, incoming_state, acknowledged_turn)
                               : classify_remote_authority(identity, current_state, incoming_state);
}

AuthorityAction HeroHistory::classify_local_authority(const HeroIdentity& identity, int current_state,
                                                      int incoming_state, std::int32_t acknowledged_turn) noexcept {
    auto& epoch = local_[identity.local_player];
    if (!epoch.identity.valid()) {
        return AuthorityAction::Apply;
    }
    if (epoch.identity != identity) {
        epoch = {};
        return AuthorityAction::Apply;
    }
    if (incoming_state == current_state) {
        if (current_state == 0 && epoch.finish_turn >= 0 && turn_after(acknowledged_turn, epoch.finish_turn)) {
            epoch = {};
        }
        return AuthorityAction::Apply;
    }
    if (incoming_state != 0 && (epoch.seen_states & state_bit(incoming_state)) != 0) {
        return AuthorityAction::SuppressHistorical;
    }
    if (incoming_state == 0 && current_state != 0 && !turn_after(acknowledged_turn, epoch.start_turn)) {
        return AuthorityAction::SuppressHistorical;
    }

    epoch = {};
    return AuthorityAction::Apply;
}

AuthorityAction HeroHistory::classify_remote_authority(const HeroIdentity& identity, int current_state,
                                                       int incoming_state) noexcept {
    auto& slot = bind_remote(identity);
    if (!slot.authority_seen) {
        seed_remote(slot, incoming_state);
        return AuthorityAction::Apply;
    }

    if (incoming_state == current_state) {
        const auto match = find_pending(slot, incoming_state);
        if (match < slot.count) {
            consume_pending(slot, match + 1);
            slot.authority_state = static_cast<std::int8_t>(incoming_state);
        } else {
            seed_remote(slot, incoming_state);
        }
        return AuthorityAction::Apply;
    }

    // A repeated authority frontier is a correction when prediction has moved elsewhere.
    if (incoming_state == slot.authority_state) {
        seed_remote(slot, incoming_state);
        return AuthorityAction::Apply;
    }

    const auto match = find_pending(slot, incoming_state);
    if (match == slot.count) {
        seed_remote(slot, incoming_state);
        return AuthorityAction::Apply;
    }

    consume_pending(slot, match + 1);
    slot.authority_state = static_cast<std::int8_t>(incoming_state);
    if (slot.count != 0 && expected_remote_state(slot) == current_state) {
        return AuthorityAction::SuppressHistorical;
    }

    seed_remote(slot, incoming_state);
    return AuthorityAction::Apply;
}

void HeroHistory::record_authority_transition(const HeroIdentity& identity, int state) noexcept {
    if (!identity.valid() || !valid_state(state)) {
        return;
    }
    auto& receipt = bind_receipt(identity);
    receipt.replay_state = state == 0 ? static_cast<std::int8_t>(-1) : static_cast<std::int8_t>(state);
    receipt.reconcile_input = identity.is_remote() && state != 0;
}

bool HeroHistory::reconcile_input(const HeroIdentity& identity, std::uint8_t buttons,
                                  std::uint8_t& down_mask) noexcept {
    if (!identity.is_remote()) {
        return false;
    }
    auto& receipt = receipts_[identity.player_handle];
    if (receipt.identity != identity || !receipt.reconcile_input) {
        return false;
    }

    receipt.reconcile_input = false;
    down_mask |= static_cast<std::uint8_t>(buttons & 0xFE);
    return true;
}

bool HeroHistory::resolve_replay(const HeroIdentity& identity, int target_state) noexcept {
    if (!identity.valid() || !valid_state(target_state)) {
        return false;
    }
    auto& receipt = receipts_[identity.player_handle];
    if (receipt.identity != identity || receipt.replay_state < 0) {
        return false;
    }

    const auto suppress = receipt.replay_state == target_state;
    receipt.replay_state = -1;
    return suppress;
}

void HeroHistory::finish_prediction(const HeroIdentity& identity) noexcept {
    if (!identity.valid()) {
        return;
    }
    auto& receipt = receipts_[identity.player_handle];
    if (receipt.identity == identity) {
        receipt.replay_state = -1;
    }
}

bool HeroHistory::local_action_active(const HeroIdentity& identity) const noexcept {
    return identity.is_local() && local_[identity.local_player].identity == identity;
}

bool HeroHistory::should_suppress_local_presentation(const HeroIdentity& identity, int current_state,
                                                     std::int32_t acknowledged_turn) const noexcept {
    if (!identity.is_local() || current_state <= 0 || current_state >= 32) {
        return false;
    }
    const auto& epoch = local_[identity.local_player];
    return epoch.identity == identity && epoch.start_turn > 0 && !turn_after(acknowledged_turn, epoch.start_turn);
}

void HeroHistory::clear(const HeroIdentity& identity) noexcept {
    if (identity.is_local()) {
        local_[identity.local_player] = {};
    }
    if (identity.player_handle >= 0 && static_cast<std::size_t>(identity.player_handle) < kNetworkPlayers) {
        remote_[identity.player_handle] = {};
        receipts_[identity.player_handle] = {};
    }
}

void HeroHistory::clear_weapon(std::uintptr_t weapon) noexcept {
    if (weapon == 0) {
        return;
    }
    for (auto& epoch : local_) {
        if (epoch.identity.weapon == weapon) {
            epoch = {};
        }
    }
    for (auto& slot : remote_) {
        if (slot.identity.weapon == weapon) {
            slot = {};
        }
    }
    for (auto& receipt : receipts_) {
        if (receipt.identity.weapon == weapon) {
            receipt = {};
        }
    }
}

void HeroHistory::clear_all() noexcept {
    local_ = {};
    remote_ = {};
    receipts_ = {};
}

HeroHistory::RemoteSlot& HeroHistory::bind_remote(const HeroIdentity& identity) noexcept {
    auto& slot = remote_[identity.player_handle];
    if (slot.identity != identity) {
        slot = {};
        slot.identity = identity;
    }
    return slot;
}

HeroHistory::AuthorityReceipt& HeroHistory::bind_receipt(const HeroIdentity& identity) noexcept {
    auto& receipt = receipts_[identity.player_handle];
    if (receipt.identity != identity) {
        receipt = {};
        receipt.identity = identity;
        receipt.replay_state = -1;
    }
    return receipt;
}

void HeroHistory::seed_remote(RemoteSlot& slot, int state) noexcept {
    slot.pending = {};
    slot.count = 0;
    slot.authority_state = static_cast<std::int8_t>(state);
    slot.authority_seen = true;
}

int HeroHistory::expected_remote_state(const RemoteSlot& slot) noexcept {
    return slot.count == 0 ? slot.authority_state : slot.pending[slot.count - 1];
}

std::size_t HeroHistory::find_pending(const RemoteSlot& slot, int state) noexcept {
    const auto end = slot.pending.begin() + slot.count;
    const auto match = std::find(slot.pending.begin(), end, static_cast<std::int8_t>(state));
    return static_cast<std::size_t>(match - slot.pending.begin());
}

void HeroHistory::consume_pending(RemoteSlot& slot, std::size_t count) noexcept {
    const auto end = slot.pending.begin() + slot.count;
    std::shift_left(slot.pending.begin(), end, static_cast<std::ptrdiff_t>(count));
    slot.count = static_cast<std::uint8_t>(slot.count - count);
}

} // namespace fusioncutter::patches::hero_animation_fix
