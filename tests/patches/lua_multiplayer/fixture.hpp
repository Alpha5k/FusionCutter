#pragma once

#include "../lua_bindings/fixture.hpp"

#include "multiplayer.hpp"

#include <fc/game.hpp>
#include <fc/multiplayer.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace fc::core_plugin::lua_multiplayer {
class Service;
}

namespace fc::tests::lua_multiplayer {

// Runs a Lua VM with the Lua provider and the runtime's multiplayer service.
struct Endpoint {
    lua::Fixture lua;
    fc::detail::CallbackCaptures captures;
    std::unique_ptr<core_plugin::lua_multiplayer::Service> messages;
    FC_MultiplayerApi multiplayer_api{};
    FC_Host host{};

    // Prepares script messaging and activates it in the fixture VM.
    Endpoint();
    // Cancels provider work before destroying the messaging handler and its callback captures.
    ~Endpoint();

    // Starts the chosen native session; connections are added explicitly by each scenario.
    void begin(GameMode);
    // Updates script observations, delivers runtime notifications and runs the provider's control-thread cleanup.
    void control();
    // Delivers local transport messages to the provider's Lua queue; tests run those handlers with lua.pump().
    void local_transport();

    runtime::MultiplayerService &network() noexcept {
        return lua.core.multiplayer();
    }
};

// Routes game packets to another endpoint's multiplayer service.
struct Link {
    Endpoint &target;
    bool receiver_is_server;
    // Native sender identity presented to the receiving runtime.
    std::uint64_t sender;

    runtime::Carrier carrier() noexcept {
        return {this, &submit};
    }

    // Delivers a native packet to the receiving runtime.
    static runtime::CarrierResult submit(void *, bool, std::uint64_t, fc::Bytes) noexcept;
};

// Connects a dedicated host and one remote client through the actual negotiation and message queues.
struct Pair {
    Endpoint host, client;
    Link from_host{client, false, 100};
    Link from_client{host, true, 200};

    // Starts both sessions and completes feature negotiation between them.
    Pair();
    // Delivers packets and control-thread callbacks; tests execute queued Lua handlers with lua.pump().
    void exchange();
};

// Records message sends and can refuse a chosen destination for the scope's lifetime.
struct SendRefusal {
    // Installs this refusal; an empty peer records sends while allowing them to continue to the runtime.
    explicit SendRefusal(PeerId);
    // Restores normal send behavior when the scope ends.
    ~SendRefusal();

    PeerId refused;
    std::vector<PeerId> attempts;
};

} // namespace fc::tests::lua_multiplayer
