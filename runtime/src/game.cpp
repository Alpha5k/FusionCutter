#include "game.hpp"

#include "fatal_boundary.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace fc::runtime {

namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

bool multiplayer(GameMode mode) noexcept {
    return mode == GameMode::Host || mode == GameMode::Client || mode == GameMode::Dedicated;
}

// Decodes the packed state: Unavailable=0, None=1, Local=2, Host=3, Client=4 and Dedicated=5.
std::optional<GameState> decode(std::uint64_t value) noexcept {
    const auto mode = value & 7;
    if (mode == 0) {
        return std::nullopt;
    }
    return mode == 1 ? GameState{} : GameState{value >> 3, static_cast<GameMode>(mode - 2)};
}

} // namespace

std::optional<GameState> GameContext::current() const noexcept {
    return decode(published_.load(std::memory_order_acquire));
}

void GameContext::start(GameMode mode) noexcept {
    // The representation reserves three bits for availability and mode.
    assert(next_session_ < (std::numeric_limits<std::uint64_t>::max() >> 3));
    active_ = {++next_session_, mode};
}

void GameContext::publish() noexcept {
    const auto mode = active_.mode == GameMode::None ? 1 : static_cast<std::uint64_t>(active_.mode) + 2;
    published_.store((active_.session << 3) | mode, std::memory_order_release);
}

void GameContext::attach(std::optional<GameMode> current_native_mode) noexcept {
    const std::lock_guard lock{producer_mutex_};
    if (established_) {
        return;
    }
    if (!current_native_mode) {
        return;
    }

    established_ = true;
    if (*current_native_mode != GameMode::None) {
        start(*current_native_mode);
    }
    publish();
}

void GameContext::observe(GameEvent event, std::optional<GameMode> current_native_mode) noexcept {
    const std::lock_guard lock{producer_mutex_};

    // End clears the session ID even when the native state cannot be read.
    if (event == GameEvent::End) {
        active_ = {};
        established_ = true;
    }

    if (!current_native_mode) {
        published_.store(0, std::memory_order_release);
        return;
    }

    switch (event) {
    case GameEvent::Start:
        assert(multiplayer(*current_native_mode));
        start(*current_native_mode);
        break;
    case GameEvent::End:
        break;
    case GameEvent::MissionEnter:
    case GameEvent::MissionExit:
        // The first mission hook can identify existing multiplayer before attach.
        // Later copied modes cannot restore an ended attempt or change the active mode.
        if (!multiplayer(active_.mode)) {
            if (multiplayer(*current_native_mode)) {
                if (!established_) {
                    start(*current_native_mode);
                }
            } else if (event == GameEvent::MissionEnter) {
                start(GameMode::Local);
            } else {
                active_ = {};
            }
        }
        break;
    }

    established_ = true;
    publish();
}

std::expected<void, Diagnostic> GameContext::on_change(PatchRecord &owner, void *capture, FC_GameChange callback) {
    if (owner.state.load(std::memory_order_acquire) != PatchState::Preparing) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Register Game notifications during prepare()"});
    }
    if (!callback) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Game notification requires a callback"});
    }
    subscriptions_.push_back({&owner, capture, callback});
    return {};
}

void GameContext::deliver_changes() {
    // Every patch in this pass receives the same state, even if a native event changes it during a callback.
    const auto value = published_.load(std::memory_order_acquire);
    const auto state = decode(value);
    const FC_GameState copied{state ? state->session : 0, state ? static_cast<std::uint32_t>(state->mode) : FC_GAME_NONE};

    for (auto &subscription : subscriptions_) {
        if (subscription.owner->state.load(std::memory_order_acquire) != PatchState::Installed || subscription.last == value) {
            continue;
        }

        // Record the delivered value before calling the patch, including the initial unavailable state.
        subscription.last = value;

        PatchCallbackScope scope{subscription.owner};
        fc::invoke_patch_callback([&] {
            subscription.callback(subscription.capture, state ? &copied : nullptr);
        });
    }
}

void GameContext::discard(PatchRecord &owner) noexcept {
    std::erase_if(subscriptions_, [&](const auto &subscription) {
        return subscription.owner == &owner;
    });
}

} // namespace fc::runtime
