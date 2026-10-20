#pragma once

#include "diagnostic.hpp"

#include <fc/game.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <vector>

namespace fc::runtime {

struct PatchRecord;

// Returns the ABI table that lets each patch read Game state and register callbacks; the table lives until process exit.
const FC_GameApi *game_api_host() noexcept;

// Marks native session and mission changes.
enum class GameEvent {
    Start,
    End,
    MissionEnter,
    MissionExit
};

// Tracks sessions from native game events and delivers changes on the control thread.
class GameContext {
  public:
    // Reads Game state from one lock-free atomic value; unavailable state has no value, while None has session zero.
    std::optional<GameState> current() const noexcept;

    // Records an existing session once, unless a native event already established it. Read native inputs before calling this method.
    // An unavailable initial read may be retried while no native event has established the session.
    void attach(std::optional<GameMode> current_native_mode) noexcept;

    // Updates session identity before notifications combine changes; Start supplies a multiplayer mode, and nullopt means unavailable.
    // Mission callbacks must supply the current native mode, so the first hook can identify existing multiplayer.
    void observe(GameEvent, std::optional<GameMode> current_native_mode) noexcept;

    // Registers a callback during Prepare. Keep its capture alive while the owner is Installed.
    // Registration, delivery and discard use the control thread; the owner remains valid through discard.
    std::expected<void, Diagnostic> on_change(PatchRecord &owner, void *capture, FC_GameChange);

    // Reads state once per control pass and calls each Installed patch initially, then on changes, attributing each call to that patch.
    void deliver_changes();

    // Removes an uninstalled patch's callbacks after handler cleanup; its owner record must remain valid through this call.
    void discard(PatchRecord &owner) noexcept;

  private:
    struct Subscription {
        PatchRecord *owner{};
        void *capture{};
        FC_GameChange callback{};
        std::optional<std::uint64_t> last;
    };

    // Assigns the next session ID while holding producer_mutex_; the ID must fit in the published value.
    void start(GameMode mode) noexcept;

    // Publishes active_ with release ordering; producer_mutex_ protects the active state and session counter.
    void publish() noexcept;

    // Serializes native session changes; readers use published_ without taking this lock.
    std::mutex producer_mutex_;
    SessionId next_session_{};
    // Keeps the session ID while state is unavailable, so recovery alone does not start a new session.
    GameState active_{};
    // Prevents a delayed attachment from replacing state already supplied by an attachment or native event.
    bool established_{};
    // The low three bits encode availability and mode; the remaining 61 bits hold the session ID.
    std::atomic<std::uint64_t> published_{};

    // The control thread owns callback registrations and their last delivered values.
    std::vector<Subscription> subscriptions_;
};

} // namespace fc::runtime
