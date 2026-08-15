#include "history.hpp"

#include <algorithm>

namespace fusioncutter::patches::hero_animation_fix {
namespace {

[[nodiscard]] constexpr bool valid_state(int state) noexcept {
    return state >= 0 && state < 32;
}

[[nodiscard]] constexpr bool turn_before(std::int32_t turn, std::int32_t reference) noexcept {
    return turn >= 0 && reference >= 0 &&
           static_cast<std::int32_t>(static_cast<std::uint32_t>(turn) - static_cast<std::uint32_t>(reference)) < 0;
}

[[nodiscard]] constexpr bool turn_after(std::int32_t turn, std::int32_t reference) noexcept {
    return turn >= 0 && reference >= 0 &&
           static_cast<std::int32_t>(static_cast<std::uint32_t>(turn) - static_cast<std::uint32_t>(reference)) > 0;
}

} // namespace

bool LocalHistory::observe_prediction(const HeroIdentity& identity, int base_state, int target_state,
                                      std::int32_t prediction_turn) noexcept {
    if (!identity.is_local() || !valid_state(base_state) || !valid_state(target_state) || base_state == target_state ||
        prediction_turn <= 0) {
        return false;
    }

    auto& slot = bind(identity);
    if (slot.base_state < 0 || slot.predicted_state != base_state ||
        (slot.count != 0 && turn_before(prediction_turn, slot.path[slot.count - 1].turn))) {
        seed(slot, identity, base_state, prediction_turn - 1);
    }
    if (slot.count == slot.path.size()) {
        slot = {};
        return false;
    }

    slot.path[slot.count++] = {
        .turn = prediction_turn,
        .to_state = static_cast<std::int8_t>(target_state),
    };
    slot.predicted_state = static_cast<std::int8_t>(target_state);
    return true;
}

AuthorityAction LocalHistory::classify_authority(const HeroIdentity& identity, int current_state, int incoming_state,
                                                 std::int32_t acknowledged_turn) noexcept {
    if (!identity.is_local() || !valid_state(current_state) || !valid_state(incoming_state) || acknowledged_turn < 0) {
        return AuthorityAction::Native;
    }

    auto& slot = bind(identity);
    if (slot.base_state < 0 || turn_before(acknowledged_turn, slot.base_turn)) {
        seed(slot, identity, incoming_state, acknowledged_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }
    if (slot.count != 0 && current_state != slot.predicted_state) {
        seed(slot, identity, incoming_state, acknowledged_turn);
        return AuthorityAction::Corrected;
    }

    auto expected_state = static_cast<int>(slot.base_state);
    std::size_t acknowledged{};
    while (acknowledged < slot.count && turn_before(slot.path[acknowledged].turn, acknowledged_turn)) {
        expected_state = slot.path[acknowledged].to_state;
        ++acknowledged;
    }

    if (incoming_state == expected_state) {
        if (acknowledged < slot.count) {
            advance(slot, acknowledged, expected_state, acknowledged_turn);
            return AuthorityAction::Historical;
        }
        const auto action = current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
        seed(slot, identity, incoming_state, acknowledged_turn);
        return action;
    }

    while (acknowledged < slot.count && slot.path[acknowledged].turn == acknowledged_turn) {
        expected_state = slot.path[acknowledged].to_state;
        ++acknowledged;
        if (incoming_state == expected_state) {
            if (acknowledged < slot.count) {
                advance(slot, acknowledged, expected_state, acknowledged_turn);
                return AuthorityAction::Historical;
            }
            const auto action = current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
            seed(slot, identity, incoming_state, acknowledged_turn);
            return action;
        }
    }

    seed(slot, identity, incoming_state, acknowledged_turn);
    return AuthorityAction::Corrected;
}

void LocalHistory::clear(const HeroIdentity& identity) noexcept {
    if (!identity.is_local()) {
        return;
    }
    auto& slot = slots_[identity.local_player];
    if (slot.identity == identity) {
        slot = {};
    }
}

void LocalHistory::clear_weapon(std::uintptr_t weapon) noexcept {
    for (auto& slot : slots_) {
        if (slot.identity.weapon == weapon) {
            slot = {};
        }
    }
}

void LocalHistory::clear_all() noexcept {
    slots_ = {};
}

LocalHistory::Slot& LocalHistory::bind(const HeroIdentity& identity) noexcept {
    auto& slot = slots_[identity.local_player];
    if (slot.identity != identity) {
        slot = {};
        slot.identity = identity;
        slot.base_state = -1;
        slot.predicted_state = -1;
        slot.base_turn = -1;
    }
    return slot;
}

void LocalHistory::seed(Slot& slot, const HeroIdentity& identity, int state, std::int32_t turn) noexcept {
    slot = {};
    slot.identity = identity;
    slot.base_state = static_cast<std::int8_t>(state);
    slot.predicted_state = static_cast<std::int8_t>(state);
    slot.base_turn = turn;
}

void LocalHistory::advance(Slot& slot, std::size_t count, int state, std::int32_t turn) noexcept {
    if (count != 0) {
        const auto end = slot.path.begin() + slot.count;
        std::shift_left(slot.path.begin(), end, static_cast<std::ptrdiff_t>(count));
        slot.count = static_cast<std::uint8_t>(slot.count - count);
    }
    slot.base_state = static_cast<std::int8_t>(state);
    slot.base_turn = turn;
    slot.predicted_state = slot.count == 0 ? slot.base_state : slot.path[slot.count - 1].to_state;
}

bool RemoteHistory::observe_prediction(const HeroIdentity& identity, int base_state, int target_state,
                                       std::int32_t update_turn) noexcept {
    if (!identity.is_remote() || !valid_state(base_state) || !valid_state(target_state) || base_state == target_state ||
        update_turn < 0) {
        return false;
    }

    auto& slot = bind(identity);
    if (!slot.authority_seen) {
        seed_authority(slot, base_state, update_turn);
    } else if (expected_state(slot) != base_state ||
               (slot.count != 0 && turn_before(update_turn, slot.path[slot.count - 1].turn))) {
        clear_pending(slot);
        seed_authority(slot, base_state, update_turn);
    }
    if (slot.count == slot.path.size()) {
        clear_pending(slot);
        return false;
    }

    slot.path[slot.count++] = {
        .turn = update_turn,
        .to_state = static_cast<std::int8_t>(target_state),
        .starts_action = base_state == 0 && target_state != 0,
    };
    slot.predicted_state = static_cast<std::int8_t>(target_state);
    return true;
}

AuthorityAction RemoteHistory::classify_authority(const HeroIdentity& identity, int current_state, int incoming_state,
                                                  std::int32_t update_turn) noexcept {
    if (!identity.is_remote() || !valid_state(current_state) || !valid_state(incoming_state) || update_turn < 0) {
        return AuthorityAction::Native;
    }

    auto& slot = bind(identity);
    if (slot.authority_seen && turn_before(update_turn, slot.authority_turn)) {
        clear_pending(slot);
        seed_authority(slot, incoming_state, update_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }
    if (!slot.authority_seen || slot.count == 0) {
        seed_authority(slot, incoming_state, update_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }

    if (incoming_state == slot.authority_state) {
        slot.authority_turn = update_turn;
        if (unconfirmed_start(slot) && incoming_state == 0) {
            const auto completion = find_pending(slot, 0);
            if (completion < slot.count) {
                consume(slot, completion + 1);
                slot.stale_idle_samples = 0;
                slot.last_stale_idle_turn = -1;
                return current_state == 0 ? AuthorityAction::Native : AuthorityAction::Historical;
            }
        }
        if (incoming_state == current_state) {
            clear_pending(slot);
            return AuthorityAction::Native;
        }
        if (unconfirmed_start(slot)) {
            const auto start_turn = slot.path[0].turn;
            if (!turn_after(update_turn, start_turn)) {
                return AuthorityAction::Historical;
            }
            if (slot.last_stale_idle_turn == update_turn) {
                return AuthorityAction::Historical;
            }
            slot.last_stale_idle_turn = update_turn;
            if (slot.stale_idle_samples++ == 0) {
                return AuthorityAction::Historical;
            }

            clear_pending(slot);
            seed_authority(slot, incoming_state, update_turn);
            return AuthorityAction::Corrected;
        }
        return AuthorityAction::Historical;
    }

    const auto match = find_pending(slot, incoming_state);
    if (match == slot.count) {
        clear_pending(slot);
        seed_authority(slot, incoming_state, update_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }

    const auto predicted_frontier = slot.predicted_state;
    slot.authority_state = static_cast<std::int8_t>(incoming_state);
    slot.authority_turn = update_turn;
    consume(slot, match + 1);
    slot.stale_idle_samples = 0;
    slot.last_stale_idle_turn = -1;
    if (current_state != predicted_frontier) {
        clear_pending(slot);
        seed_authority(slot, incoming_state, update_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }
    return slot.count == 0 && current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Historical;
}

void RemoteHistory::begin_authority(const HeroIdentity& identity) noexcept {
    if (auto* slot = find(identity); slot != nullptr) {
        slot->replay_state = -1;
        slot->prediction_window = false;
    }
}

void RemoteHistory::observe_authority_application(const HeroIdentity& identity, int current_state,
                                                  int incoming_state) noexcept {
    if (!identity.is_remote() || current_state == incoming_state || incoming_state == 0) {
        return;
    }

    auto& slot = bind(identity);
    slot.replay_state = static_cast<std::int8_t>(incoming_state);
}

void RemoteHistory::begin_prediction(const HeroIdentity& identity) noexcept {
    auto& slot = bind(identity);
    slot.prediction_window = slot.replay_state >= 0;
}

bool RemoteHistory::reconcile_input(const HeroIdentity& identity, std::uint8_t buttons,
                                    std::uint8_t& down_mask) noexcept {
    if (!identity.is_remote()) {
        return false;
    }

    auto& slot = bind(identity);
    constexpr std::uint8_t kMeleeButtons = 0xFE;
    const auto current_buttons = static_cast<std::uint8_t>(buttons & kMeleeButtons);
    const auto repair = slot.previous_buttons_valid
                            ? static_cast<std::uint8_t>(current_buttons & slot.previous_buttons & ~down_mask)
                            : 0;
    slot.previous_buttons = current_buttons;
    slot.previous_buttons_valid = true;

    const auto reconciled = static_cast<std::uint8_t>(down_mask | repair);
    const auto changed = reconciled != down_mask;
    down_mask = reconciled;
    return changed;
}

bool RemoteHistory::resolve_replay(const HeroIdentity& identity, int target_state) noexcept {
    if (!identity.is_remote() || !valid_state(target_state)) {
        return false;
    }
    auto* slot = find(identity);
    if (slot == nullptr || !slot->prediction_window || slot->replay_state < 0) {
        return false;
    }

    const auto replay = slot->replay_state == target_state;
    slot->replay_state = -1;
    slot->prediction_window = false;
    return replay;
}

void RemoteHistory::finish_prediction(const HeroIdentity& identity) noexcept {
    if (auto* slot = find(identity); slot != nullptr) {
        slot->replay_state = -1;
        slot->prediction_window = false;
    }
}

void RemoteHistory::clear(const HeroIdentity& identity) noexcept {
    if (!identity.is_remote()) {
        return;
    }
    auto& slot = slots_[identity.player_handle];
    if (slot.identity == identity) {
        slot = {};
    }
}

void RemoteHistory::clear_weapon(std::uintptr_t weapon) noexcept {
    for (auto& slot : slots_) {
        if (slot.identity.weapon == weapon) {
            slot = {};
        }
    }
}

void RemoteHistory::clear_all() noexcept {
    slots_ = {};
}

RemoteHistory::Slot& RemoteHistory::bind(const HeroIdentity& identity) noexcept {
    auto& slot = slots_[identity.player_handle];
    if (slot.identity != identity) {
        slot = {};
        slot.identity = identity;
        slot.predicted_state = -1;
        slot.authority_state = -1;
        slot.authority_turn = -1;
        slot.last_stale_idle_turn = -1;
        slot.replay_state = -1;
    }
    return slot;
}

RemoteHistory::Slot* RemoteHistory::find(const HeroIdentity& identity) noexcept {
    if (!identity.is_remote()) {
        return nullptr;
    }
    auto& slot = slots_[identity.player_handle];
    return slot.identity == identity ? &slot : nullptr;
}

void RemoteHistory::seed_authority(Slot& slot, int state, std::int32_t turn) noexcept {
    slot.authority_seen = true;
    slot.authority_state = static_cast<std::int8_t>(state);
    slot.authority_turn = turn;
    slot.predicted_state = static_cast<std::int8_t>(state);
    slot.stale_idle_samples = 0;
    slot.last_stale_idle_turn = -1;
}

void RemoteHistory::clear_pending(Slot& slot) noexcept {
    slot.path = {};
    slot.count = 0;
    slot.predicted_state = slot.authority_state;
    slot.stale_idle_samples = 0;
    slot.last_stale_idle_turn = -1;
}

void RemoteHistory::consume(Slot& slot, std::size_t count) noexcept {
    const auto end = slot.path.begin() + slot.count;
    std::shift_left(slot.path.begin(), end, static_cast<std::ptrdiff_t>(count));
    slot.count = static_cast<std::uint8_t>(slot.count - count);
    slot.predicted_state = slot.count == 0 ? slot.authority_state : slot.path[slot.count - 1].to_state;
}

std::size_t RemoteHistory::find_pending(const Slot& slot, int state) noexcept {
    const auto end = slot.path.begin() + slot.count;
    const auto found = std::find_if(slot.path.begin(), end, [state](const StateNode& node) {
        return node.to_state == state;
    });
    return static_cast<std::size_t>(found - slot.path.begin());
}

int RemoteHistory::expected_state(const Slot& slot) noexcept {
    return slot.count == 0 ? slot.authority_state : slot.path[slot.count - 1].to_state;
}

bool RemoteHistory::unconfirmed_start(const Slot& slot) noexcept {
    return slot.authority_state == 0 && slot.count != 0 && slot.path[0].starts_action;
}

} // namespace fusioncutter::patches::hero_animation_fix
