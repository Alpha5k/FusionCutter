#include "multiplayer.hpp"

#include "core_runtime.hpp"
#include "message_protocol.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"

#include <fc/contexts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using fc::Agreement;
using fc::Bytes;
using fc::FeatureRole;
using fc::GameMode;
using fc::PeerId;
using fc::SendResult;
using fc::runtime::Carrier;
using fc::runtime::CarrierResult;
using fc::runtime::MultiplayerService;
using fc::runtime::PatchRecord;
using fc::runtime::PatchState;
namespace wire = fc::runtime::message_protocol;

// Gives each test patch stable declarations and API reservations while it prepares its features.
struct Owner {
    FC_Plugin declaration{};
    FC_Patch definition{};
    fc::runtime::PluginRecord plugin;
    PatchRecord record;

    // Prepares stable patch declarations and registration reservations.
    Owner() {
        declaration.id = {"Fixture", 7};
        definition.id = {"Messages", 8};
        plugin.admitted.definition = &declaration;
        record.plugin = &plugin;
        record.definition = &definition;
        record.state = PatchState::Preparing;
        record.api = std::make_unique<fc::runtime::PatchApiReservations>(record);
    }

    // Registers a feature with the production service before the test activates this patch.
    FC_Feature &feature(MultiplayerService &service, std::string_view id = "example.v1", FeatureRole role = FeatureRole::Both) {
        auto result = service.feature(record, id, static_cast<std::uint32_t>(role));
        REQUIRE(result);
        return **result;
    }
};

// Records submission attempts and copies accepted frames for later delivery.
struct Transport {
    struct Frame {
        std::uint64_t recipient;
        std::vector<std::byte> bytes;
    };

    CarrierResult result = CarrierResult::Accepted;
    std::vector<Frame> attempts, accepted;

    // Returns the fixture's native frame submission adapter.
    Carrier carrier() {
        return {this, &submit};
    }

    // Copies the submitted frame before returning the configured transport result.
    static CarrierResult submit(void *context, bool, std::uint64_t recipient, Bytes bytes) noexcept {
        auto &transport = *static_cast<Transport *>(context);
        transport.attempts.push_back({recipient, {bytes.begin(), bytes.end()}});

        if (transport.result == CarrierResult::Accepted) {
            transport.accepted.push_back(transport.attempts.back());
        }
        return transport.result;
    }

    // Delivers accepted frames using the supplied sender ID at a later servicing opportunity.
    void flush(MultiplayerService &receiver, bool server, std::uint64_t sender, MultiplayerService::Clock::time_point now) {
        auto frames = std::move(accepted);
        accepted.clear();
        for (const auto &frame : frames) {
            receiver.receive(server, sender, frame.bytes, now);
        }
    }
};

// Uses the production codec and two service instances representing a remote host and client.
struct Pair {
    Owner host_owner, client_owner;
    MultiplayerService host, client;
    FC_Feature &host_feature = host_owner.feature(host);
    FC_Feature &client_feature = client_owner.feature(client);
    Transport from_host, from_client;
    MultiplayerService::Clock::time_point now = MultiplayerService::Clock::now();

    // Allows extra registration before activation, then starts the host/client sessions and their native connections.
    explicit Pair(std::function<void(Pair &)> prepare = {}) {
        if (prepare) {
            prepare(*this);
        }

        host_owner.record.state = client_owner.record.state = PatchState::Installed;
        host.begin({1, GameMode::Dedicated}, true);
        client.begin({2, GameMode::Client}, true);
        host.connect(0, 200, 200);
        client.connect(64, 999, 100);
    }

    // Passes Hello, Welcome and Ready between the two services to establish their connection.
    void handshake() {
        client.pump(from_client.carrier(), now);
        from_client.flush(host, true, 200, now);
        host.pump(from_host.carrier(), now);
        from_host.flush(client, false, 100, now);
        client.pump(from_client.carrier(), now);
        from_client.flush(host, true, 200, now);
    }
};

// Copies callback data and checks patch attribution; an optional reply tests sending from a receive callback.
struct Delivery {
    PatchRecord *owner{};
    MultiplayerService *service{};
    FC_Feature *reply{};
    std::vector<std::vector<std::byte>> messages;
    std::vector<PeerId> senders;
    bool attributed = true;
    SendResult reply_result = SendResult::Unavailable;

    // Copies the complete payload while valid and optionally sends a reply from the callback.
    static void FC_CDECL message(void *context, std::uint64_t id, FC_Bytes bytes) noexcept {
        auto &delivery = *static_cast<Delivery *>(context);
        delivery.attributed &= fc::runtime::current_consumer() == delivery.owner;
        const Bytes payload{reinterpret_cast<const std::byte *>(bytes.data), bytes.size};
        delivery.messages.emplace_back(payload.begin(), payload.end());
        delivery.senders.push_back({id});

        if (delivery.reply) {
            delivery.reply_result = delivery.service->send(*delivery.reply, {id}, payload);
        }
    }
};

// Encodes a complete control frame into fixture-owned storage.
std::vector<std::byte> control(wire::Kind kind, wire::Tokens tokens, std::span<const std::string_view> ids = {}) {
    std::array<std::byte, wire::kFrameBytes> storage;
    const auto size = kind == wire::Kind::Ready ? wire::encode_ready(tokens, storage) : wire::encode_manifest(kind, tokens, ids, storage);
    REQUIRE(size);
    return {storage.begin(), storage.begin() + size};
}
} // namespace

TEST_CASE("Multiplayer separates host and local peers and expires native connection identities", "[multiplayer][service]") {
    MultiplayerService service;
    Owner client, host;
    auto &client_feature = client.feature(service, "example.v1", FeatureRole::Client);
    auto &host_feature = host.feature(service, "example.v1", FeatureRole::Host);
    CHECK_FALSE(service.feature(host.record, "example.v1", FC_FEATURE_BOTH));
    client.record.state = host.record.state = PatchState::Installed;
    service.begin({1, GameMode::Host}, true);
    service.connect(0, 0, std::nullopt, true);
    const auto host_peer = service.host(), local = service.local();
    CHECK(host_peer != local);
    CHECK(service.clients() == std::vector{local});
    CHECK(service.agreement(client_feature, host_peer) == Agreement::Compatible);
    CHECK(service.agreement(host_feature, local) == Agreement::Compatible);
    CHECK(service.agreement(client_feature, local) == Agreement::Unavailable);

    service.disconnect(0);
    CHECK_FALSE(service.info(local));
    service.connect(0, 0, std::nullopt, true);
    CHECK(service.local() != local);
    service.reset_connections();
    CHECK(service.host().value == 0);
    service.connect(0, 0, std::nullopt, true);
    CHECK(service.host() != host_peer);

    service.end();
    CHECK(service.clients().empty());
    service.begin({2, GameMode::Dedicated}, true);
    service.observe_local({std::nullopt, 456});
    REQUIRE(service.host().value);
    CHECK(service.local().value == 0);
    CHECK(service.info(service.host())->galaxy_id == 456ull);
}

TEST_CASE("Local messages and callback replies stay queued and identify the receiving patch", "[multiplayer][service]") {
    MultiplayerService service;
    Owner client, host;
    auto &client_feature = client.feature(service, "example.v1", FeatureRole::Client);
    auto &host_feature = host.feature(service, "example.v1", FeatureRole::Host);
    auto &one_sided = client.feature(service, "client_only.v1", FeatureRole::Client);
    Delivery to_host{&host.record, &service, &host_feature}, to_client{&client.record};
    REQUIRE(service.on_message(host_feature, &to_host, &Delivery::message));
    REQUIRE(service.on_message(client_feature, &to_client, &Delivery::message));

    client.record.state = host.record.state = PatchState::Installed;
    service.begin({1, GameMode::Host}, true);
    service.connect(0, 0, std::nullopt, true);
    const auto local = service.local(), host_id = service.host();
    CHECK(service.agreement(one_sided, host_id) == Agreement::Incompatible);

    std::array payload{std::byte{1}, std::byte{0}, std::byte{3}};
    REQUIRE(service.send(client_feature, host_id, payload) == SendResult::Accepted);
    payload[0] = std::byte{9};
    service.deliver();
    CHECK(to_host.messages.empty());
    service.pump({}, MultiplayerService::Clock::now());
    CHECK(to_host.messages.empty());

    service.deliver();
    REQUIRE(to_host.messages.size() == 1);
    CHECK(to_host.messages[0][0] == std::byte{1});
    CHECK(to_host.senders[0] == local);
    CHECK(to_host.reply_result == SendResult::Accepted);
    CHECK(to_client.messages.empty());

    service.deliver();
    CHECK(to_client.messages.empty());
    service.pump({}, MultiplayerService::Clock::now());
    service.deliver();
    REQUIRE(to_client.messages.size() == 1);
    CHECK(to_client.senders[0] == host_id);
    CHECK(to_client.attributed);
    CHECK(to_host.attributed);
}

TEST_CASE("Remote agreement requires an accepted Ready frame and rejects stale tokens or late confirmation", "[multiplayer][service]") {
    Delivery received;
    Pair pair([&](Pair &preparing) {
        received.owner = &preparing.client_owner.record;
        REQUIRE(preparing.client.on_message(preparing.client_feature, &received, &Delivery::message));
    });
    const auto host_peer = pair.client.host(), client_peer = pair.host.clients()[0];
    CHECK(pair.client.clients().empty());
    CHECK(pair.client.local() != host_peer);
    CHECK(pair.client.info(host_peer)->galaxy_id == 100);
    pair.client.observe_local({0, 200});
    CHECK(pair.client.info(pair.client.local())->slot == 0u);

    pair.client.pump(pair.from_client.carrier(), pair.now);
    const auto hello = pair.from_client.accepted[0].bytes;
    pair.from_client.flush(pair.host, true, 200, pair.now);
    pair.host.pump(pair.from_host.carrier(), pair.now);
    const auto welcome = pair.from_host.accepted[0].bytes;
    const auto tokens = wire::parse(welcome)->tokens;
    pair.from_host.flush(pair.client, false, 100, pair.now);

    pair.from_client.result = CarrierResult::Retry;
    pair.client.pump(pair.from_client.carrier(), pair.now);
    CHECK(pair.client.agreement(pair.client_feature, host_peer) == Agreement::Pending);
    CHECK(pair.host.agreement(pair.host_feature, client_peer) == Agreement::Pending);
    const auto refused_ready = pair.from_client.attempts.back().bytes;
    std::array<std::byte, wire::kFrameBytes> data_frame;
    std::array payload{std::byte{5}};
    const auto data_size = wire::encode_data(tokens, "example.v1", payload, data_frame);
    pair.client.receive(false, 100, {data_frame.data(), data_size}, pair.now);
    pair.client.deliver();
    CHECK(received.messages.empty());

    pair.from_client.result = CarrierResult::Accepted;
    pair.client.pump(pair.from_client.carrier(), pair.now);
    CHECK(pair.from_client.attempts.back().bytes == refused_ready);
    CHECK(pair.client.agreement(pair.client_feature, host_peer) == Agreement::Compatible);
    pair.client.deliver();
    REQUIRE(received.messages.size() == 1);
    CHECK(received.messages[0] == std::vector{std::byte{5}});
    pair.from_client.flush(pair.host, true, 200, pair.now);
    CHECK(pair.host.agreement(pair.host_feature, client_peer) == Agreement::Compatible);

    const auto count = pair.from_client.attempts.size();
    pair.client.pump(pair.from_client.carrier(), pair.now);
    CHECK(pair.from_client.attempts.size() == count);

    pair.host.connect(0, 200, 201);
    CHECK(pair.host.clients()[0] == client_peer);

    pair.host.disconnect(0);
    pair.host.connect(0, 200, 200);
    const auto replacement = pair.host.clients()[0];
    CHECK(replacement != client_peer);

    pair.host.receive(true, 200, hello, pair.now);
    pair.host.pump(pair.from_host.carrier(), pair.now);
    const auto fresh = wire::parse(pair.from_host.accepted.back().bytes)->tokens;
    CHECK(fresh.host != tokens.host);

    pair.host.receive(true, 200, control(wire::Kind::Ready, tokens), pair.now);
    CHECK(pair.host.agreement(pair.host_feature, replacement) == Agreement::Pending);
    pair.host.pump(pair.from_host.carrier(), pair.now + std::chrono::seconds(4));
    CHECK(pair.host.agreement(pair.host_feature, replacement) == Agreement::Incompatible);

    pair.host.receive(true, 200, control(wire::Kind::Ready, fresh), pair.now + std::chrono::seconds(4));
    CHECK(pair.host.agreement(pair.host_feature, replacement) == Agreement::Incompatible);
}

TEST_CASE("A replacement Hello refreshes remote support and rejects the old Ready", "[multiplayer][service]") {
    Pair pair;
    const std::array ids{std::string_view{"example.v1"}, std::string_view{"unmatched.v1"}};
    const auto peer = pair.host.clients()[0];
    pair.host.receive(true, 200, control(wire::Kind::Hello, {1, 0}, ids), pair.now);
    pair.host.pump(pair.from_host.carrier(), pair.now);
    REQUIRE(pair.from_host.accepted.size() == 1);
    const auto old_tokens = wire::parse(pair.from_host.accepted.back().bytes)->tokens;

    pair.host.receive(true, 200, control(wire::Kind::Hello, {2, 0}), pair.now);
    pair.host.pump(pair.from_host.carrier(), pair.now);
    REQUIRE(pair.from_host.accepted.size() == 2);
    const auto replacement = wire::parse(pair.from_host.accepted.back().bytes)->tokens;
    CHECK(replacement.host != old_tokens.host);
    pair.host.receive(true, 200, control(wire::Kind::Ready, old_tokens), pair.now);
    CHECK(pair.host.agreement(pair.host_feature, peer) == Agreement::Pending);
    pair.host.receive(true, 200, control(wire::Kind::Ready, replacement), pair.now);
    CHECK(pair.host.agreement(pair.host_feature, peer) == Agreement::Incompatible);
    CHECK(pair.host.send(pair.host_feature, peer, {}) == SendResult::Incompatible);
}

TEST_CASE("Hello and Welcome retries retain the sorted feature lists frozen at Game Start", "[multiplayer][service]") {
    Pair pair([](Pair &preparing) {
        preparing.host_owner.feature(preparing.host, "a.host", FeatureRole::Host);
        preparing.client_owner.feature(preparing.client, "a.client", FeatureRole::Client);
    });
    Owner later_host, later_client;
    later_host.feature(pair.host, "late.host", FeatureRole::Host);
    later_client.feature(pair.client, "late.client", FeatureRole::Client);
    later_host.record.state = later_client.record.state = PatchState::Installed;

    pair.from_client.result = CarrierResult::Retry;
    pair.client.pump(pair.from_client.carrier(), pair.now);
    REQUIRE(pair.from_client.attempts.size() == 1);
    const auto hello = wire::parse(pair.from_client.attempts.back().bytes);
    REQUIRE(hello);
    CHECK(hello->manifest == std::vector<std::string_view>{"a.client", "example.v1"});
    const auto refused_hello = pair.from_client.attempts.back().bytes;
    pair.from_client.result = CarrierResult::Accepted;
    pair.client.pump(pair.from_client.carrier(), pair.now);
    CHECK(pair.from_client.attempts.back().bytes == refused_hello);
    pair.from_client.flush(pair.host, true, 200, pair.now);

    pair.from_host.result = CarrierResult::Retry;
    pair.host.pump(pair.from_host.carrier(), pair.now);
    REQUIRE(pair.from_host.attempts.size() == 1);
    const auto welcome = wire::parse(pair.from_host.attempts.back().bytes);
    REQUIRE(welcome);
    CHECK(welcome->manifest == std::vector<std::string_view>{"a.host", "example.v1"});
    const auto refused_welcome = pair.from_host.attempts.back().bytes;
    pair.from_host.result = CarrierResult::Accepted;
    pair.host.pump(pair.from_host.carrier(), pair.now);
    CHECK(pair.from_host.attempts.back().bytes == refused_welcome);

    // A new session admits the later registrations and still generates a fresh client token.
    pair.host.begin({3, GameMode::Dedicated}, true);
    pair.client.begin({4, GameMode::Client}, true);
    pair.host.connect(0, 200, 200);
    pair.client.connect(64, 999, 100);
    pair.from_host.accepted.clear();
    pair.client.pump(pair.from_client.carrier(), pair.now);
    const auto next_hello = wire::parse(pair.from_client.accepted.back().bytes);
    REQUIRE(next_hello);
    CHECK(next_hello->tokens.client != wire::parse(refused_hello)->tokens.client);
    CHECK(next_hello->manifest == std::vector<std::string_view>{"a.client", "example.v1", "late.client"});
    pair.from_client.flush(pair.host, true, 200, pair.now);
    pair.host.pump(pair.from_host.carrier(), pair.now);
    const auto next_welcome = wire::parse(pair.from_host.accepted.back().bytes);
    REQUIRE(next_welcome);
    CHECK(next_welcome->manifest == std::vector<std::string_view>{"a.host", "example.v1", "late.host"});
}

TEST_CASE("An early Hello completes negotiation after its native sender connects", "[multiplayer][service]") {
    Pair pair;
    pair.host.reset_connections();
    pair.client.pump(pair.from_client.carrier(), pair.now);
    REQUIRE(pair.from_client.accepted.size() == 1);
    pair.from_client.flush(pair.host, true, 200, pair.now);
    CHECK(pair.host.clients().empty());
    CHECK(pair.host.host().value == 0);
    pair.host.pump(pair.from_host.carrier(), pair.now);
    CHECK(pair.from_host.attempts.empty());

    pair.host.connect(0, 200, 200);
    const auto peer = pair.host.clients()[0];
    pair.now += std::chrono::milliseconds(100);
    pair.host.pump(pair.from_host.carrier(), pair.now);
    REQUIRE(pair.from_host.accepted.size() == 1);
    CHECK(pair.from_host.accepted[0].recipient == 200);
    pair.from_host.flush(pair.client, false, 100, pair.now);
    pair.client.pump(pair.from_client.carrier(), pair.now);
    pair.from_client.flush(pair.host, true, 200, pair.now);
    CHECK(pair.host.agreement(pair.host_feature, peer) == Agreement::Compatible);
    CHECK(pair.client.agreement(pair.client_feature, pair.client.host()) == Agreement::Compatible);
}

TEST_CASE("Remote delivery retries a refused frame and isolates feature failures until transport fails", "[multiplayer][service]") {
    Delivery received;
    FC_Feature *other_host{}, *other_client{};
    Pair pair([&](Pair &preparing) {
        other_host = &preparing.host_owner.feature(preparing.host, "other.v1");
        other_client = &preparing.client_owner.feature(preparing.client, "other.v1");
        received.owner = &preparing.host_owner.record;
        REQUIRE(preparing.host.on_message(preparing.host_feature, &received, &Delivery::message));
    });
    pair.handshake();
    const auto peer = pair.client.host(), client = pair.host.clients()[0];
    const std::array payload{std::byte{4}, std::byte{0}, std::byte{8}};
    REQUIRE(pair.client.send(pair.client_feature, peer, payload) == SendResult::Accepted);
    pair.from_client.result = CarrierResult::Retry;
    pair.client.pump(pair.from_client.carrier(), pair.now);
    const auto refused = pair.from_client.attempts.back().bytes;
    pair.from_client.result = CarrierResult::Accepted;
    pair.client.pump(pair.from_client.carrier(), pair.now);
    REQUIRE(pair.from_client.accepted.size() == 1);
    CHECK(pair.from_client.accepted[0].bytes == refused);
    pair.from_client.flush(pair.host, true, 200, pair.now);
    CHECK(received.messages.empty());
    pair.host.deliver();
    REQUIRE(received.messages.size() == 1);
    CHECK(std::ranges::equal(received.messages[0], payload));
    CHECK(received.senders[0] == client);
    CHECK(received.attributed);

    pair.client.fail(pair.client_feature, peer, "Fixture delivery failed");
    CHECK(pair.client.agreement(pair.client_feature, peer) == Agreement::Failed);
    CHECK(pair.client.agreement(*other_client, peer) == Agreement::Compatible);
    pair.client.pump(pair.from_client.carrier(), pair.now);
    pair.from_client.flush(pair.host, true, 200, pair.now);
    CHECK(pair.host.agreement(pair.host_feature, client) == Agreement::Failed);
    CHECK(pair.host.agreement(*other_host, client) == Agreement::Compatible);

    REQUIRE(pair.client.send(*other_client, peer, payload) == SendResult::Accepted);
    pair.from_client.result = CarrierResult::Failed;
    pair.client.pump(pair.from_client.carrier(), pair.now);
    CHECK(pair.client.send(*other_client, peer, payload) == SendResult::Failed);
    pair.host.carrier_failed("Fixture transport failed");
    CHECK(pair.host.agreement(*other_host, client) == Agreement::Failed);
}

TEST_CASE("Prepared Multiplayer handles retain callbacks and broadcast to compatible clients", "[multiplayer][service]") {
    fc::runtime::CoreRuntime runtime{{}};
    Owner owner;
    owner.record.runtime = &runtime;
    fc::detail::CallbackCaptures captures;
    fc::Multiplayer multiplayer;
    fc::Feature feature;
    std::vector<std::byte> received;
    PeerId sender;
    std::vector<Agreement> agreements;
    bool attributed = true;
    auto lifetime = std::make_shared<int>(7);
    const std::weak_ptr<int> retained = lifetime;
    {
        fc::PrepareContext prepare{{&owner.record, &runtime.host()}, &captures};
        multiplayer = prepare.multiplayer();
        auto registered = multiplayer.feature({"example.v1"});
        REQUIRE(registered);
        feature = *registered;
        REQUIRE(feature.on_peer([lifetime, &agreements, &attributed, &owner](PeerId, Agreement agreement) noexcept {
            attributed &= fc::runtime::current_consumer() == &owner.record;
            agreements.push_back(agreement);
        }));
        REQUIRE(feature.on_message([&](PeerId peer, Bytes bytes) noexcept {
            attributed &= fc::runtime::current_consumer() == &owner.record;
            sender = peer;
            received.assign(bytes.begin(), bytes.end());
        }));
    }
    lifetime.reset();
    CHECK_FALSE(retained.expired());
    runtime.multiplayer().deliver();
    CHECK(agreements.empty());

    owner.record.state = PatchState::Installed;
    runtime.multiplayer().begin({1, GameMode::Host}, true);
    runtime.multiplayer().connect(0, 0, std::nullopt, true);
    runtime.multiplayer().connect(1, 200, 200);
    runtime.multiplayer().deliver();
    CHECK(std::ranges::count(agreements, Agreement::Compatible) == 2);
    const auto local = multiplayer.local();
    CHECK(multiplayer.host() != local);
    CHECK(feature.agreement(local) == Agreement::Compatible);
    CHECK(feature.agreement(multiplayer.clients().back()) == Agreement::Pending);
    const std::array payload{std::byte{3}, std::byte{0}, std::byte{9}};
    REQUIRE(feature.broadcast(payload));
    CHECK(received.empty());
    runtime.multiplayer().pump({}, MultiplayerService::Clock::now());
    runtime.multiplayer().deliver();
    CHECK(std::ranges::equal(received, payload));
    CHECK(sender == multiplayer.host());
    CHECK(attributed);

    runtime.multiplayer().end();
    CHECK(feature.broadcast(payload));
    owner.record.state = PatchState::Failed;
    runtime.multiplayer().discard(owner.record);
    captures.clear();
    CHECK(retained.expired());
}
