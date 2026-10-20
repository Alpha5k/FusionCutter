#include "game.hpp"

#include "core_runtime.hpp"
#include "patches/installation.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"

#include <fc/contexts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace {
using fc::GameMode;
using fc::runtime::GameContext;
using fc::runtime::GameEvent;

// Checks the current mode and returns the session ID for comparisons across native events.
fc::SessionId session(const GameContext &game, GameMode mode) {
    const auto state = game.current();
    REQUIRE(state);
    CHECK(state->mode == mode);
    return state->session;
}

// Keeps notification observations alive after a failed handler releases its SDK captures.
struct Observation {
    fc::runtime::PatchRecord *owner{};
    std::thread::id control_thread = std::this_thread::get_id();
    std::optional<fc::GameState> state;
    unsigned calls{};
    bool attributed = true;
    bool on_control_thread = true;
    bool destroyed{};
};

// Records the delivered state, calling thread and patch identity assigned by the runtime.
auto changed(Observation &observation) {
    return [&observation](std::optional<fc::GameState> state) noexcept {
        ++observation.calls;
        observation.state = state;
        observation.attributed &= fc::runtime::current_consumer() == observation.owner;
        observation.on_control_thread &= std::this_thread::get_id() == observation.control_thread;
    };
}

// Retains SDK capture storage in the same handler that the installer destroys after rejected preparation.
struct Handler {
    fc::detail::StageAccess access;
    fc::detail::CallbackCaptures captures;
    Observation *observation;
};

// Builds a handler that ignores a registration error to check whether the installer still rejects it.
std::uint32_t FC_CDECL create_failed(void *context, void *owner, const FC_Host *host, const FC_Value *, std::uint32_t, FC_Handler *out,
                                     FC_ErrorSink) {
    *out = {};
    out->context = new Handler{{owner, host}, {}, static_cast<Observation *>(context)};
    out->prepare = [](void *context, FC_ErrorSink) -> std::uint32_t {
        auto &handler = *static_cast<Handler *>(context);
        auto game = fc::PrepareContext{handler.access, &handler.captures}.game();

        // Register a valid callback first so failed preparation also has capture storage to release.
        if (!game.on_change(changed(*handler.observation))) {
            return 0;
        }

        handler.access.host->game->on_change(handler.access.owner, nullptr, nullptr, {});
        return 1;
    };

    // Observe handler destruction before the installer removes failed registrations.
    out->destroy = [](void *context) {
        std::unique_ptr<Handler> handler{static_cast<Handler *>(context)};
        handler->observation->destroyed = true;
    };
    return 1;
}

} // namespace

TEST_CASE("Game state distinguishes None from unavailable and preserves the session ID on recovery", "[runtime][game]") {
    GameContext game;
    CHECK_FALSE(game.current());

    game.attach(GameMode::None);
    CHECK(session(game, GameMode::None) == 0);
    game.observe(GameEvent::Start, GameMode::Client);
    const auto client = session(game, GameMode::Client);
    CHECK(client != 0);

    game.observe(GameEvent::MissionExit, std::nullopt);
    CHECK_FALSE(game.current());
    game.observe(GameEvent::MissionEnter, GameMode::Client);
    CHECK(session(game, GameMode::Client) == client);

    game.observe(GameEvent::End, GameMode::None);
    CHECK(session(game, GameMode::None) == 0);
}

TEST_CASE("Every Local mission run receives a new session ID", "[runtime][game]") {
    GameContext game;
    game.attach(GameMode::Local);
    const auto attached = session(game, GameMode::Local);
    CHECK(attached != 0);

    game.observe(GameEvent::MissionEnter, GameMode::Local);
    const auto restarted = session(game, GameMode::Local);
    CHECK(restarted > attached);
    game.observe(GameEvent::MissionExit, GameMode::None);
    CHECK(session(game, GameMode::None) == 0);
    game.observe(GameEvent::MissionEnter, GameMode::None);
    CHECK(session(game, GameMode::Local) > restarted);
}

TEST_CASE("Multiplayer attempts start before missions and preserve the session ID across maps", "[runtime][game]") {
    GameContext game;
    fc::SessionId previous{};
    for (const auto mode : {GameMode::Host, GameMode::Client, GameMode::Dedicated}) {
        game.observe(GameEvent::Start, mode);
        const auto canceled = session(game, mode);
        CHECK(canceled > previous);
        game.observe(GameEvent::End, GameMode::None);
        CHECK(session(game, GameMode::None) == 0);

        game.observe(GameEvent::Start, mode);
        const auto active = session(game, mode);
        CHECK(active > canceled);
        game.observe(GameEvent::MissionEnter, mode);
        game.observe(GameEvent::MissionExit, mode);
        game.observe(GameEvent::MissionEnter, mode);
        CHECK(session(game, mode) == active);

        // End and Start replace the session ID even if the control thread never reads the intermediate None state.
        game.observe(GameEvent::End, GameMode::None);
        game.observe(GameEvent::Start, mode);
        previous = session(game, mode);
        CHECK(previous > active);
        game.observe(GameEvent::End, GameMode::None);
    }
}

TEST_CASE("Native events prevent late attachment and stale mission modes from restoring an old session", "[runtime][game]") {
    GameContext game;
    game.observe(GameEvent::MissionEnter, GameMode::Dedicated);
    const auto attached = session(game, GameMode::Dedicated);
    game.attach(GameMode::Local);
    CHECK(session(game, GameMode::Dedicated) == attached);

    game.observe(GameEvent::End, GameMode::None);
    game.attach(GameMode::Dedicated);
    game.observe(GameEvent::MissionEnter, GameMode::Dedicated);
    CHECK(session(game, GameMode::None) == 0);

    game.observe(GameEvent::Start, GameMode::Client);
    const auto replacement = session(game, GameMode::Client);
    game.observe(GameEvent::MissionEnter, GameMode::Dedicated);
    CHECK(session(game, GameMode::Client) == replacement);
}

TEST_CASE("Game callbacks begin after activation and run on the control thread under the patch's identity", "[runtime][game]") {
    fc::runtime::CoreRuntime runtime{{}};
    fc::runtime::PatchRecord owner;
    owner.runtime = &runtime;
    owner.api = std::make_unique<fc::runtime::PatchApiReservations>(owner);
    owner.state = fc::runtime::PatchState::Preparing;
    fc::detail::CallbackCaptures captures;
    Observation observation{&owner};
    fc::Game game;
    {
        fc::PrepareContext prepare{{&owner, &runtime.host()}, &captures};
        game = prepare.game();
    }

    CHECK_FALSE(game.current());
    REQUIRE(game.on_change(changed(observation)));
    runtime.game().deliver_changes();
    CHECK(observation.calls == 0);

    owner.state = fc::runtime::PatchState::Installed;
    runtime.game().deliver_changes();
    CHECK(observation.calls == 1);
    CHECK_FALSE(observation.state);

    runtime.game().attach(fc::GameMode::Client);
    const auto snapshot = game.current();
    REQUIRE(snapshot);
    CHECK(snapshot->mode == fc::GameMode::Client);
    CHECK(snapshot->session != 0);

    runtime.game().deliver_changes();
    REQUIRE(observation.state);
    CHECK(observation.state->session == snapshot->session);

    runtime.game().deliver_changes();
    CHECK(observation.calls == 2);
    runtime.game().observe(GameEvent::End, GameMode::None);
    CHECK(snapshot->mode == fc::GameMode::Client);
    CHECK(session(runtime.game(), GameMode::None) == 0);
    CHECK(observation.attributed);
    CHECK(observation.on_control_thread);
    CHECK(fc::runtime::current_consumer() == nullptr);
}

TEST_CASE("A Game registration error rejects preparation and releases captures even when the handler ignores it", "[runtime][game]") {
    fc::runtime::CoreRuntime runtime{{}};
    fc::runtime::PatchRecord owner;
    Observation observation{&owner};
    owner.runtime = &runtime;
    owner.api = std::make_unique<fc::runtime::PatchApiReservations>(owner);
    owner.resolved.owner = "Game consumer";
    owner.internal_factory = {&observation, &create_failed};
    owner.selection.state = fc::runtime::SelectionState::Ready;
    owner.state = fc::runtime::PatchState::Planning;
    fc::runtime::SelectionSession session{{}, FC_ROLE_CLIENT};
    const std::array private_owners{&owner};

    fc::runtime::install_ready_patches(session, {}, {&runtime.host(), nullptr, nullptr, nullptr, private_owners});
    CHECK(owner.state == fc::runtime::PatchState::Failed);
    REQUIRE(owner.api->error());
    CHECK(observation.destroyed);

    runtime.game().attach(fc::GameMode::Client);
    runtime.game().deliver_changes();
    CHECK(observation.calls == 0);
}
