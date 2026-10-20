#pragma once

#include <fc/base.hpp>
#include <fc/detail/native_types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace fc {

using SessionId = std::uint64_t;

// Describes actual local participation; LAN sessions use the multiplayer modes.
enum class GameMode {
    Local = FC_GAME_LOCAL,         // Singleplayer mission.
    Host = FC_GAME_HOST,           // Hosting multiplayer with a local player.
    Client = FC_GAME_CLIENT,       // Joining or playing on another host.
    Dedicated = FC_GAME_DEDICATED, // Hosting multiplayer without a local player.
    None = FC_GAME_NONE            // No active session.
};

// Contains Game state copied from the runtime; keeping this value does not keep it current.
struct GameState {
    // Process-local monotonic ID for a local mission run or multiplayer attempt; zero means no active session.
    SessionId session = 0;
    GameMode mode = GameMode::None;

    bool is_host() const noexcept {
        return mode == GameMode::Host || mode == GameMode::Dedicated;
    }

    bool is_client() const noexcept {
        return mode == GameMode::Client;
    }
};

namespace detail {

// Returns the host's game table when its published ABI prefix includes the field.
inline const FC_GameApi *game_service(const FC_Host *host) noexcept {
    return table_available(host, offsetof(FC_Host, game), sizeof(host->game)) ? host->game : nullptr;
}

// Copies the supplied C state to an SDK value; null means unavailable state.
inline std::optional<GameState> game_state(const FC_GameState *state) noexcept {
    return state ? std::optional{GameState{state->session, static_cast<GameMode>(state->mode)}} : std::nullopt;
}

// Keeps the callback in the handler's capture storage so the consumer DLL destroys it.
template <class F> struct GameCapture final : CallbackCapture {
    explicit GameCapture(F value) : function(std::move(value)) {}

    F function;

    // Copies the supplied C state before calling the patch's noexcept callback.
    static void FC_CDECL invoke(void *context, const FC_GameState *state) noexcept {
        auto &capture = *static_cast<GameCapture *>(context);
        std::invoke(capture.function, game_state(state));
    }
};
} // namespace detail

// Provides Game state to a patch; the handle may be retained after Prepare.
class Game {
  public:
    Game() = default;

    explicit Game(detail::StageAccess access, detail::CallbackCaptures *captures = nullptr) noexcept
        : access_(access), captures_(captures) {}

    // Copies Game state on any thread with a bounded, nonblocking read; nullopt means unavailable state.
    // When no session is active, the returned state has mode None and session zero.
    std::optional<GameState> current() const noexcept {
        const auto *api = detail::game_service(access_.host);
        if (!detail::table_available(api, offsetof(FC_GameApi, current), sizeof(api->current)) || !api->current) {
            return std::nullopt;
        }
        FC_GameState state{};
        return api->current(access_.owner, &state) == 1 ? detail::game_state(&state) : std::nullopt;
    }

    // Registers a noexcept callback during Prepare and keeps it in this DLL; the first call follows patch activation.
    // The control thread serializes it with the patch's other callbacks; rapid changes may be combined.
    template <class F> Result<> on_change(F &&function) const {
        using Capture = detail::GameCapture<std::decay_t<F>>;
        static_assert(std::is_nothrow_invocable_v<std::decay_t<F> &, std::optional<GameState>>,
                      "Game change callbacks must accept an optional GameState without throwing (noexcept)");
        const auto *api = detail::game_service(access_.host);
        if (!captures_ || !detail::table_available(api, offsetof(FC_GameApi, on_change), sizeof(api->on_change)) || !api->on_change) {
            return std::unexpected(Error{"Game change notifications are unavailable"});
        }

        // Retain the callable before giving its stable address to the runtime.
        auto capture = std::make_unique<Capture>(std::forward<F>(function));
        auto *context = capture.get();
        captures_->push_back(std::move(capture));

        detail::NativeError error;
        if (api->on_change(access_.owner, context, &Capture::invoke, error.sink()) != 1) {
            // The runtime keeps no callback after refusal; remove this capture and preserve earlier registrations.
            captures_->pop_back();
            return std::unexpected(error.value.message.empty() ? Error{"Could not register for Game changes"} : error.value);
        }

        return {};
    }

  private:
    detail::StageAccess access_{};
    detail::CallbackCaptures *captures_{};
};
} // namespace fc
