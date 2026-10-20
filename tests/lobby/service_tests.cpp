#include "lobby.hpp"

#include "core_runtime.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"

#include <fc/contexts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using fc::LobbyId;
using fc::LobbyRecord;
using fc::LobbyResult;
using fc::runtime::Diagnostic;
using fc::runtime::LobbyBackend;
using fc::runtime::LobbyNativeState;
using fc::runtime::LobbyService;
using fc::runtime::PatchRecord;
using fc::runtime::PatchState;
using Token = std::unique_ptr<FC_LobbyRequest, void(FC_CDECL *)(FC_LobbyRequest *)>;
using namespace std::chrono_literals;

// Keeps patch declarations and Prepare errors alive for calls through the runtime API.
struct Owner {
    FC_Plugin declaration{};
    FC_Patch definition{};
    fc::runtime::PluginRecord plugin;
    PatchRecord record;

    // Gives the preparing patch stable plugin and item ownership records.
    Owner() {
        declaration.id = {"Fixture", 7};
        definition.id = {"Lobby", 5};
        plugin.admitted.definition = &declaration;
        record.plugin = &plugin;
        record.definition = &definition;
        record.state = PatchState::Preparing;
        record.api = std::make_unique<fc::runtime::PatchApiReservations>(record);
    }
};

// Supplies native state and text, records requests and writes, and leaves completion under test control.
struct Native {
    // Records a native value write or key removal for the fixture.
    struct Write {
        std::uint64_t lobby;
        std::string key;
        std::optional<std::string> value;
    };

    LobbyNativeState snapshot{true, 0, 0};
    std::map<std::pair<std::uint64_t, std::string>, std::string> values;
    std::vector<std::uint64_t> requests;
    std::vector<Write> writes;
    std::size_t reads{};
    bool refuse_request{}, refuse_write{};

    // Supplies the production service with this fixture's native adapter.
    LobbyBackend backend() {
        return {this, &state, &request, &read, &write};
    }

    static LobbyNativeState state(void *context) noexcept {
        return static_cast<Native *>(context)->snapshot;
    }

    // Records each native request and reports the configured immediate failure.
    static std::expected<void, Diagnostic> request(void *context, std::uint64_t lobby) {
        auto &native = *static_cast<Native *>(context);
        native.requests.push_back(lobby);
        return native.refuse_request ? std::unexpected(Diagnostic{"native", "Request failed"}) : std::expected<void, Diagnostic>{};
    }

    // Supplies native text when pump calls it; complete() only records the listener's result.
    static std::expected<std::string, Diagnostic> read(void *context, std::uint64_t lobby, std::string_view key) {
        auto &native = *static_cast<Native *>(context);
        ++native.reads;
        const auto found = native.values.find({lobby, std::string{key}});
        return found == native.values.end() ? std::string{} : found->second;
    }

    // Records a native write, distinguishing key removal from an empty FC value.
    static std::expected<void, Diagnostic> write(void *context, std::uint64_t lobby, std::string_view key,
                                                 std::optional<std::string_view> value) {
        auto &native = *static_cast<Native *>(context);
        native.writes.push_back({lobby, std::string{key}, value ? std::optional<std::string>{*value} : std::nullopt});
        return native.refuse_write ? std::unexpected(Diagnostic{"native", "Write failed"}) : std::expected<void, Diagnostic>{};
    }
};

// Copies completed records and checks attribution to the requesting patch.
struct Delivery {
    PatchRecord *owner{};
    std::vector<LobbyRecord> records;
    bool attributed = true;

    // Copies the borrowed record while it is valid under the requesting patch identity.
    static void FC_CDECL read(void *context, const FC_LobbyRecord *record) noexcept {
        auto &delivery = *static_cast<Delivery *>(context);
        delivery.attributed &= fc::runtime::current_consumer() == delivery.owner;
        delivery.records.push_back(fc::detail::lobby_record(*record));
    }
};

// Keeps native access and the patch alive until the service is destroyed; request tokens use the runtime C cancel function.
struct Fixture {
    Owner owner;
    Native native;
    LobbyService service;
    LobbyService::Clock::time_point now{100s};

    // Configures native key/value limits and keeps the backend attached for the test.
    Fixture() {
        service.configure(1023, 4095);
        service.attach(native.backend());
    }

    // Registers an item for the fixture's preparing patch.
    FC_LobbyItem &item(std::string_view name = "example", std::uint32_t version = 1) {
        auto registered = service.item(owner.record, name, version);
        REQUIRE(registered);
        return **registered;
    }

    // Keeps an accepted request token; native work and callback delivery happen in later service passes.
    Token refresh(Delivery &delivery, LobbyId lobby = {77}, std::string_view name = "example", std::uint32_t version = 1) {
        auto accepted = service.refresh(owner.record, lobby, name, version, &delivery, &Delivery::read);
        REQUIRE(accepted);
        return {*accepted, fc::runtime::lobby_api_host()->cancel};
    }

    void pump() {
        service.pump(now);
    }

    // Returns a copied SDK record if the requested observation is cached.
    std::optional<LobbyRecord> cached(LobbyId lobby = {77}, std::string_view name = "example", std::uint32_t version = 1) {
        fc::detail::LobbyCopy copy;
        return service.cached(lobby, name, version, &copy, &fc::detail::LobbyCopy::invoke) ? std::move(copy.record) : std::nullopt;
    }
};
} // namespace

TEST_CASE("Lobby copies accepted values, writes only to owned lobbies and skips unchanged values", "[lobby][service]") {
    Fixture fixture;
    auto &item = fixture.item();
    Owner other;
    CHECK_FALSE(fixture.service.item(other.record, "example", 2));
    CHECK_FALSE(fixture.service.item(other.record, {}, 1));

    fixture.owner.record.state = PatchState::Installed;
    std::array<std::byte, 3> bytes{std::byte{1}, std::byte{0}, std::byte{3}};
    REQUIRE(fixture.service.publish(item, bytes));
    bytes.fill(std::byte{});

    fixture.native.snapshot.current = 55;
    fixture.pump();
    CHECK(fixture.native.writes.empty());
    CHECK(fixture.service.current() == LobbyId{55});

    fixture.native.snapshot.owned = 88;
    fixture.pump();
    REQUIRE(fixture.native.writes.size() == 1);
    CHECK(fixture.native.writes[0].lobby == 88);
    CHECK(fixture.native.writes[0].key == "FC.example");
    CHECK(fixture.native.writes[0].value == "FC1:1:AQAD");

    fixture.pump();
    CHECK(fixture.native.writes.size() == 1);
    REQUIRE(fixture.service.publish(item, {}));
    fixture.now += 500ms;
    fixture.pump();
    CHECK(fixture.native.writes.size() == 1);
    fixture.now += 500ms;
    fixture.pump();
    REQUIRE(fixture.native.writes.size() == 2);
    CHECK(fixture.native.writes.back().value == "FC1:1:");

    fixture.service.clear(item);
    fixture.now += 1s;
    fixture.pump();
    REQUIRE(fixture.native.writes.size() == 3);
    CHECK_FALSE(fixture.native.writes.back().value);
    fixture.service.clear(item);
    fixture.now += 1s;
    fixture.pump();
    CHECK(fixture.native.writes.size() == 3);
}

TEST_CASE("Lobby readers share one native request and compare versions against the same cached value", "[lobby][service]") {
    Fixture fixture;
    fixture.owner.record.state = PatchState::Installed;
    Delivery first{&fixture.owner.record}, second{&fixture.owner.record}, missing{&fixture.owner.record};
    fixture.native.values[{77, "FC.example"}] = "FC1:3:CQA=";
    auto first_token = fixture.refresh(first, {77}, "example", 3);
    auto second_token = fixture.refresh(second, {77}, "example", 4);
    auto missing_token = fixture.refresh(missing, {77}, "absent", 3);
    CHECK(fixture.native.requests.empty());
    CHECK_FALSE(fixture.cached());

    fixture.pump();
    CHECK(fixture.native.requests == std::vector<std::uint64_t>{77});

    CHECK(fixture.service.complete(77, true));
    CHECK(fixture.native.reads == 0);
    CHECK(first.records.empty());

    fixture.pump();
    CHECK(fixture.native.reads == 2);
    CHECK(first.records.empty());

    fixture.service.deliver();
    REQUIRE(first.records.size() == 1);
    REQUIRE(second.records.size() == 1);
    REQUIRE(missing.records.size() == 1);
    CHECK(first.records[0].result == LobbyResult::Value);
    CHECK(first.records[0].value == std::vector{std::byte{9}, std::byte{0}});
    CHECK(second.records[0].result == LobbyResult::Unsupported);
    CHECK(second.records[0].version == 3);
    CHECK(second.records[0].value.empty());
    CHECK(missing.records[0].result == LobbyResult::Missing);
    CHECK(first.attributed);
    CHECK(second.attributed);

    const auto retained = fixture.cached({77}, "example", 3);
    REQUIRE(retained);
    CHECK(retained->observed == fixture.now);

    fixture.now += 1s;
    Delivery failure{&fixture.owner.record};
    auto failed_token = fixture.refresh(failure, {77}, "example", 3);
    fixture.pump();
    CHECK(fixture.service.complete(77, false));
    fixture.pump();
    fixture.service.deliver();
    REQUIRE(failure.records.size() == 1);
    CHECK(failure.records[0].result == LobbyResult::Failed);
    CHECK(failure.records[0].value.empty());
    CHECK(failure.records[0].observed == LobbyService::Clock::time_point{});
    const auto older = fixture.cached({77}, "example", 3);
    REQUIRE(older);
    CHECK(older->observed == retained->observed);
    CHECK(older->value == retained->value);
}

TEST_CASE("Lobby decoding distinguishes malformed records and unsupported envelopes", "[lobby][service]") {
    const std::pair<std::string, LobbyResult> cases[]{{"FC1:1:AB==", LobbyResult::Malformed},
                                                      {"FC2:1:", LobbyResult::Unsupported},
                                                      {"FC1:1:", LobbyResult::Value},
                                                      {"", LobbyResult::Missing}};
    for (const auto &[text, result] : cases) {
        INFO(text);
        Fixture fixture;
        fixture.owner.record.state = PatchState::Installed;
        Delivery delivery{&fixture.owner.record};
        fixture.native.values[{77, "FC.example"}] = text;
        auto token = fixture.refresh(delivery);
        fixture.pump();
        fixture.service.complete(77, true);
        fixture.pump();
        fixture.service.deliver();
        REQUIRE(delivery.records.size() == 1);
        CHECK(delivery.records[0].result == result);
        CHECK(delivery.records[0].value.empty());
        CHECK(delivery.records[0].observed == fixture.now);
    }
}

TEST_CASE("Lobby cancellation preserves submitted requests and invalidation waits for old completions", "[lobby][service]") {
    Fixture fixture;
    fixture.owner.record.state = PatchState::Installed;
    Delivery canceled{&fixture.owner.record}, shared{&fixture.owner.record}, replacement{&fixture.owner.record};
    auto old = fixture.refresh(canceled);
    fixture.pump();
    old.reset();
    auto sharing = fixture.refresh(shared);
    fixture.pump();
    CHECK(fixture.native.requests.size() == 1);

    fixture.service.invalidate();
    auto fresh = fixture.refresh(replacement);
    fixture.pump();
    CHECK(fixture.native.requests.size() == 1);
    fixture.service.deliver();
    CHECK(canceled.records.empty());
    REQUIRE(shared.records.size() == 1);
    CHECK(shared.records[0].result == LobbyResult::Unavailable);
    CHECK(replacement.records.empty());

    CHECK(fixture.service.complete(77, true));
    CHECK_FALSE(fixture.service.complete(77, false));
    fixture.pump();
    CHECK(fixture.native.requests.size() == 2);
    CHECK(fixture.native.reads == 0);
    fixture.service.deliver();
    CHECK(replacement.records.empty());

    fixture.native.values[{77, "FC.example"}] = "FC1:1:Bw==";
    fixture.service.complete(77, true);
    fixture.pump();
    fixture.service.deliver();
    REQUIRE(replacement.records.size() == 1);
    CHECK(replacement.records[0].value == std::vector{std::byte{7}});
}

TEST_CASE("Lobby retries failed publication and reports a refused native read request", "[lobby][service]") {
    Fixture fixture;
    auto &item = fixture.item();
    REQUIRE(fixture.service.publish(item, {}));
    fixture.owner.record.state = PatchState::Installed;
    fixture.native.snapshot.owned = 77;
    fixture.native.refuse_write = true;
    fixture.pump();
    fixture.native.refuse_write = false;
    fixture.now += 1s;
    fixture.pump();
    REQUIRE(fixture.native.writes.size() == 2);
    CHECK(fixture.native.writes.back().value == "FC1:1:");

    Delivery failure{&fixture.owner.record};
    fixture.native.refuse_request = true;
    auto token = fixture.refresh(failure);
    fixture.pump();
    // Native refusal is recorded during submission and completed on the next service pass.
    fixture.pump();
    fixture.service.deliver();
    REQUIRE(failure.records.size() == 1);
    CHECK(failure.records[0].result == LobbyResult::Failed);
}

TEST_CASE("Prepared Lobby requests cancel delivery and keep captures alive while callbacks release them", "[lobby][service]") {
    Native native;
    fc::runtime::CoreRuntime runtime{{}};
    runtime.lobby().configure(1023, 4095);
    runtime.lobby().attach(native.backend());
    Owner owner;
    owner.record.runtime = &runtime;
    fc::Lobby lobby;
    fc::LobbyItem item;
    {
        fc::PrepareContext prepare{{&owner.record, &runtime.host()}};
        lobby = prepare.lobby();
        auto registered = lobby.item("example", 1);
        REQUIRE(registered);
        item = *registered;
    }
    owner.record.state = PatchState::Installed;
    REQUIRE(item.publish({}));
    native.snapshot.owned = 55;
    auto now = LobbyService::Clock::now();
    runtime.lobby().pump(now);
    REQUIRE(native.writes.size() == 1);
    CHECK(native.writes[0].value == "FC1:1:");

    unsigned canceled_calls{};
    auto canceled = lobby.refresh({77}, "example", 1, [&](LobbyRecord) noexcept {
        ++canceled_calls;
    });
    REQUIRE(canceled);
    fc::Request moved{std::move(*canceled)};
    runtime.lobby().pump(now);
    moved = fc::Request{};
    runtime.lobby().complete(77, true);
    runtime.lobby().pump(now);
    runtime.lobby().deliver();
    CHECK(canceled_calls == 0);

    native.values[{77, "FC.example"}] = "FC1:1:CQA=";
    fc::Request self;
    std::optional<LobbyRecord> received;
    auto lifetime = std::make_shared<int>(7);
    const std::weak_ptr<int> retained = lifetime;
    bool alive_after_release{}, attributed{};
    auto accepted = lobby.refresh(
        {77}, "example", 1,
        [value = std::move(lifetime), &self, &received, &retained, &alive_after_release, &attributed, &owner](LobbyRecord record) noexcept {
            self = fc::Request{};
            alive_after_release = !retained.expired();
            attributed = fc::runtime::current_consumer() == &owner.record;
            received = std::move(record);
        });
    REQUIRE(accepted);
    self = std::move(*accepted);
    CHECK_FALSE(received);
    runtime.lobby().pump(now);
    REQUIRE(runtime.lobby().complete(77, true));
    runtime.lobby().pump(now);
    runtime.lobby().deliver();
    REQUIRE(received);
    CHECK(received->value == std::vector{std::byte{9}, std::byte{0}});
    CHECK(alive_after_release);
    CHECK(retained.expired());
    CHECK(attributed);
    const auto cached = lobby.cached({77}, "example", 1);
    REQUIRE(cached);
    CHECK(cached->value == received->value);
}
