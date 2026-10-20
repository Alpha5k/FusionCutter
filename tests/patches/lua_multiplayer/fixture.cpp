#include "fixture.hpp"

#include "../../../patches/lua_multiplayer/service.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

namespace fc::tests::lua_multiplayer {
namespace {

SendRefusal *refusal{};

// Records matching sends and applies the selected refusal before forwarding accepted attempts to the runtime.
std::uint32_t FC_CDECL send(FC_Feature *feature, std::uint64_t peer, FC_Bytes bytes) {
    if (refusal && feature->id == "FC.LuaMessages" && bytes.size && bytes.data[0] == 2) {
        refusal->attempts.push_back({peer});
        if (refusal->refused.value == peer) {
            return FC_SEND_FULL;
        }
    }

    return runtime::multiplayer_api_host()->send(feature, peer, bytes);
}

} // namespace

Endpoint::Endpoint() : messages(std::make_unique<core_plugin::lua_multiplayer::Service>()) {
    // Replace just the send entry so scenarios can refuse one destination while retaining the runtime's other behavior.
    multiplayer_api = *runtime::multiplayer_api_host();
    multiplayer_api.send = &send;
    host = lua.core.host();
    host.multiplayer = &multiplayer_api;

    fc::PrepareContext context{{&lua.consumer, &host}, &captures};
    REQUIRE(messages->prepare(context));

    lua.start();
    lua.core.game().attach(GameMode::None);
    control();
}

Endpoint::~Endpoint() {
    // Notify the messaging patch while its handler still exists, then retire every capture referring to it.
    lua.retire();
    lua.control();

    lua.consumer.state = runtime::PatchState::Failed;
    lua.control();
    lua.core.multiplayer().discard(lua.consumer);
    lua.core.game().discard(lua.consumer);

    messages.reset();
    captures.clear();
}

void Endpoint::begin(GameMode mode) {
    lua.core.game().observe(runtime::GameEvent::Start, mode);
    const auto state = lua.core.game().current();
    REQUIRE(state);

    network().begin(*state, true);
    network().observe_local({});
    control();
}

void Endpoint::control() {
    // Match production: patch updates precede native peer and message callbacks.
    messages->update();
    network().deliver();
    lua.control();
}

void Endpoint::local_transport() {
    network().pump({}, runtime::MultiplayerService::Clock::now());
    control();
}

runtime::CarrierResult Link::submit(void *context, bool, std::uint64_t, fc::Bytes bytes) noexcept {
    auto &link = *static_cast<Link *>(context);
    link.target.network().receive(link.receiver_is_server, link.sender, bytes, runtime::MultiplayerService::Clock::now());
    return runtime::CarrierResult::Accepted;
}

Pair::Pair() {
    host.begin(GameMode::Dedicated);
    client.begin(GameMode::Client);
    host.network().connect(0, 200, 200);
    client.network().connect(64, 999, 100);

    // Advance negotiation and the initial listener lists through both transport and control callbacks.
    for (unsigned i = 0; i != 4; ++i) {
        exchange();
    }

    const auto clients = host.network().clients();
    REQUIRE(clients.size() == 1);
}

void Pair::exchange() {
    host.network().pump(from_host.carrier(), runtime::MultiplayerService::Clock::now());
    client.network().pump(from_client.carrier(), runtime::MultiplayerService::Clock::now());

    host.control();
    client.control();
}

SendRefusal::SendRefusal(PeerId peer) : refused(peer) {
    REQUIRE(refusal == nullptr);
    refusal = this;
}

SendRefusal::~SendRefusal() {
    refusal = nullptr;
}

} // namespace fc::tests::lua_multiplayer
