#include "fixture.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using fc::tests::lua_multiplayer::Endpoint;
using fc::tests::lua_multiplayer::Pair;

} // namespace

TEST_CASE("Lua multiplayer preserves peer identity and cancels replaced or closed local listeners", "[lua][multiplayer]") {
    Endpoint endpoint;
    endpoint.begin(fc::GameMode::Host);
    endpoint.network().connect(0, 0, std::nullopt, true);
    endpoint.lua.script(R"(
        M = FusionCutter.Multiplayer
        Received = 0
        assert(M.Listen("scores", function(sender, value)
            LastSender = sender
            assert(value.text == "a\000b" and value[true] == 12)
            Received = Received + 1
        end))
    )");
    for (unsigned i = 0; i != 3; ++i) {
        endpoint.local_transport();
    }

    endpoint.lua.script(R"(
        assert(M.IsHost() and not M.IsClient())
        SavedHost = M.Host()
        SavedClient = M.Clients()[1]
        assert(type(SavedHost) == "userdata" and SavedHost == M.Host())
        assert(SavedClient == M.Clients()[1] and SavedHost ~= SavedClient)
        local keyed = {[SavedHost] = "host", [SavedClient] = "client"}
        assert(keyed[M.Host()] == "host" and keyed[M.Clients()[1]] == "client")
        assert(M.Ready(SavedHost, "scores") and M.Ready(SavedClient, "scores"))
        assert(M.SendHost("scores", {text = "a\000b", [true] = 12}) == "accepted")
        assert(Received == 0)
    )");

    endpoint.local_transport();
    endpoint.lua.script("assert(Received == 0)");
    endpoint.lua.pump();
    endpoint.control();
    endpoint.lua.script(R"(
        assert(Received == 1 and LastSender == SavedClient)
        assert(M.SendClient(SavedClient, "scores", {text = "a\000b", [true] = 12}) == "accepted")
    )");
    endpoint.local_transport();
    endpoint.lua.pump();
    endpoint.control();
    endpoint.lua.script("assert(Received == 2 and LastSender == SavedHost)");

    endpoint.lua.script("assert(M.SendHost('scores', nil) == 'accepted')");
    endpoint.local_transport();
    endpoint.lua.script("assert(M.Listen('scores', function() Received = Received + 10 end))");
    endpoint.lua.pump();
    endpoint.control();
    endpoint.lua.script("assert(Received == 2)");

    // The new listener is usable after peers receive its registration.
    for (unsigned i = 0; i != 2; ++i) {
        endpoint.local_transport();
    }
    endpoint.lua.script("assert(M.SendHost('scores', nil) == 'accepted')");
    endpoint.local_transport();
    endpoint.lua.pump();
    endpoint.control();
    endpoint.lua.script("assert(Received == 12 and M.SendHost('scores', nil) == 'accepted')");

    endpoint.local_transport();
    endpoint.lua.script("assert(M.Close('scores'))");
    endpoint.lua.pump();
    endpoint.control();
    endpoint.lua.script("assert(Received == 12)");
    CHECK(lua_gettop(endpoint.lua.vm.get()) == 0);
}

TEST_CASE("Disconnecting cancels queued Lua messages; reconnecting creates new peer identities", "[lua][multiplayer]") {
    Pair pair;
    pair.host.lua.script(R"(
        M = FusionCutter.Multiplayer
        Received = 0
        assert(M.Listen("inbox", function() Received = Received + 1 end))
    )");
    pair.client.lua.script("M = FusionCutter.Multiplayer; OldHost = M.Host()");
    pair.exchange();
    pair.exchange();
    pair.client.lua.script("assert(M.SendHost('inbox', nil) == 'accepted')");
    pair.exchange();

    pair.host.network().disconnect(0);
    pair.client.network().disconnect(64);
    pair.host.control();
    pair.client.control();
    pair.host.lua.pump();
    pair.host.lua.script("assert(Received == 0)");

    pair.host.network().connect(0, 200, 200);
    pair.client.network().connect(64, 999, 100);
    for (unsigned i = 0; i != 4; ++i) {
        pair.exchange();
    }
    pair.client.lua.script(R"(
        assert(OldHost ~= M.Host())
        assert(not M.Ready(OldHost, "inbox"))
        assert(M.Ready(M.Host(), "inbox") and M.SendHost("inbox", nil) == "accepted")
    )");
    pair.exchange();
    pair.host.lua.pump();
    pair.host.control();
    pair.host.lua.script("assert(Received == 1)");
}

TEST_CASE("Lua Broadcast attempts every client and returns false if any send is refused", "[lua][multiplayer]") {
    using fc::tests::lua_multiplayer::Link;
    using fc::tests::lua_multiplayer::SendRefusal;
    Pair pair;
    Endpoint second;
    second.begin(fc::GameMode::Client);
    pair.host.network().connect(1, 300, 300);
    second.network().connect(64, 999, 100);
    Link second_to_host{pair.host, true, 300};

    // Routes each host packet to the corresponding client service.
    struct Routes {
        Link first, second;

        // Selects the client by its native recipient ID before forwarding the packet through Link.
        static fc::runtime::CarrierResult submit(void *context, bool server, std::uint64_t recipient, fc::Bytes bytes) noexcept {
            auto &routes = *static_cast<Routes *>(context);
            auto &link = recipient == 200 ? routes.first : routes.second;
            return Link::submit(&link, server, recipient, bytes);
        }
    } routes{{pair.client, false, 100}, {second, false, 100}};

    // Service all three endpoints before their control callbacks observe the delivered packets.
    const auto exchange = [&] {
        const auto now = fc::runtime::MultiplayerService::Clock::now();
        pair.host.network().pump({&routes, &Routes::submit}, now);
        pair.client.network().pump(pair.from_client.carrier(), now);
        second.network().pump(second_to_host.carrier(), now);

        pair.host.control();
        pair.client.control();
        second.control();
    };

    for (auto *client : {&pair.client, &second}) {
        client->lua.script(R"(
            M = FusionCutter.Multiplayer
            Received = 0
            assert(M.Listen("broadcast", function() Received = Received + 1 end))
        )");
    }

    for (unsigned i = 0; i != 4; ++i) {
        exchange();
    }

    const auto peers = pair.host.network().clients();
    REQUIRE(peers.size() == 2);
    {
        SendRefusal refuse{peers.front()};
        pair.host.lua.script("assert(not FusionCutter.Multiplayer.Broadcast('broadcast', {value = 7}))");
        CHECK(refuse.attempts == peers);
    }

    exchange();
    pair.client.lua.pump();
    second.lua.pump();
    pair.client.lua.script("assert(Received == 0)");
    second.lua.script("assert(Received == 1)");
}

TEST_CASE("Lua frame failures prevent packet sends and listener calls", "[lua][multiplayer][frame]") {
    Pair pair;
    pair.host.lua.script(R"(
        Received = 0
        assert(FusionCutter.Multiplayer.Listen("inbox", function() Received = Received + 1 end))
    )");
    pair.client.lua.script("M = FusionCutter.Multiplayer");
    pair.exchange();
    pair.exchange();
    pair.client.lua.script("assert(M.Ready(M.Host(), 'inbox'))");

    SECTION("A failed traversal cannot send its completed prefix") {
        fc::tests::lua_multiplayer::SendRefusal observe{{}};
        // Iteration reserves twice for begin and the first entry; refuse the next advance.
        fc::tests::lua::StackReserveFailure failure{pair.client.lua.vm.get(), 2, 2};
        pair.client.lua.script(R"(
            local ok, reason = pcall(M.SendHost, "inbox", {first = 1, second = 2})
            assert(not ok and string.find(reason, "reserve stack space", 1, true))
        )");
        REQUIRE(failure.triggered());
        CHECK(observe.attempts.empty());
    }

    SECTION("A failed decoded string cannot reach its listener") {
        pair.client.lua.script("assert(M.SendHost('inbox', 'payload') == 'accepted')");
        pair.exchange();
        // The first value construction in the deferred callback is the decoded string.
        fc::tests::lua::StackReserveFailure failure{pair.host.lua.vm.get(), 5};
        pair.host.lua.pump();
        REQUIRE(failure.triggered());
        pair.host.lua.script("assert(Received == 0)");
    }
    CHECK(lua_gettop(pair.host.lua.vm.get()) == 0);
    CHECK(lua_gettop(pair.client.lua.vm.get()) == 0);
}
