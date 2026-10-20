#include "patches/patch_api.hpp"
#include "core_runtime.hpp"
#include "patches/patch_record.hpp"

#include <fc/contexts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <WinSock2.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using fc::runtime::Diagnostic;
using fc::runtime::PatchApiContract;
using fc::runtime::PatchApiReservations;
using fc::runtime::PatchRecord;
using fc::runtime::PatchState;

// Real stable records provide the identity and dependency edges consumed by the API implementation.
struct Owners {
    fc::runtime::CoreRuntime runtime{{}};
    FC_Plugin declaration{};
    FC_Patch provider_definition{}, consumer_definition{}, other_definition{};
    fc::runtime::PluginRecord plugin;
    fc::runtime::SelectionSession session{{}, FC_ROLE_CLIENT};
    PatchRecord &provider = add_owner();
    PatchRecord &consumer = add_owner();
    PatchRecord &other = add_owner();

    // Adds a heap-owned session record so later additions cannot invalidate its identity.
    PatchRecord &add_owner() {
        session.patches.push_back(std::make_unique<PatchRecord>());
        return *session.patches.back();
    }

    // Gives each reservation a stable declaration and grants only the consumer's provider dependency.
    Owners() {
        declaration.id = {"Example", 7};
        plugin.admitted.definition = &declaration;
        provider_definition.id = {"Provider", 8};
        consumer_definition.id = {"Consumer", 8};
        other_definition.id = {"Other", 5};
        provider.plugin = consumer.plugin = other.plugin = &plugin;
        provider.definition = &provider_definition;
        consumer.definition = &consumer_definition;
        other.definition = &other_definition;
        provider.runtime = consumer.runtime = other.runtime = &runtime;
        provider.state = consumer.state = other.state = PatchState::Preparing;

        consumer.providers.push_back(&provider);
        provider.api = std::make_unique<PatchApiReservations>(provider);
        consumer.api = std::make_unique<PatchApiReservations>(consumer);
        other.api = std::make_unique<PatchApiReservations>(other);
    }
};

// An aligned table exercises copied storage beyond the default allocation alignment.
struct alignas(64) CounterTable {
    void *context{};
    std::uint32_t(FC_CDECL *read)(void *) noexcept {};
    std::array<std::byte, 64 - 2 * sizeof(void *)> payload{};
};

// Reads the counter borrowed by a table; context must point to a live uint32_t.
std::uint32_t FC_CDECL read_counter(void *context) noexcept {
    return *static_cast<const std::uint32_t *>(context);
}

constexpr std::array kDescription{std::byte{1}, std::byte{2}, std::byte{3}};
constexpr std::array<std::uint32_t, 1> kFunctions{offsetof(CounterTable, read)};

// Returns a borrowed contract for CounterTable; id must outlive the reservation call using it.
PatchApiContract contract(std::string_view id = "example.counter.v1") {
    return {id, sizeof(CounterTable), alignof(CounterTable), kDescription, kFunctions};
}

// Copies a connected table into typed storage; input must contain a complete CounterTable.
CounterTable copy_table(std::span<const std::byte> input) {
    CounterTable result;
    std::memcpy(&result, input.data(), sizeof(result));
    return result;
}

// Retains notification results for assertions after synchronous delivery.
struct Delivery {
    std::uint32_t calls{}, value{};
};

// Invokes the delivered counter and records its result in the consumer-owned Delivery context.
void FC_CDECL available(void *context, const void *table) noexcept {
    auto &delivery = *static_cast<Delivery *>(context);
    const auto &counter = *static_cast<const CounterTable *>(table);
    ++delivery.calls;
    delivery.value = counter.read(counter.context);
}

// Records the thread and attributed owner while deliberately changing both thread-local error values.
struct Consumer {
    std::uint32_t calls{};
    std::thread::id thread;
    fc::runtime::PatchRecord *origin{};

    // Records callback execution and changes both thread-local errors before returning a recognizable result.
    std::uint32_t invoke(std::uint32_t value) noexcept {
        ++calls;
        thread = std::this_thread::get_id();
        origin = fc::runtime::current_consumer();
        WSASetLastError(4321);
        SetLastError(1234);
        return value + 7;
    }
};
} // namespace

TEST_CASE("Patch API reservations copy temporary tables and contracts before activation", "[runtime][patch-api]") {
    Owners owners;
    auto &provider = *owners.provider.api;
    auto &consumer = *owners.consumer.api;
    std::uint32_t value = 42;

    {
        std::string id = "example.counter.v1";
        std::vector description(kDescription.begin(), kDescription.end());
        std::vector offsets(kFunctions.begin(), kFunctions.end());
        const PatchApiContract temporary{id, sizeof(CounterTable), alignof(CounterTable), description, offsets};
        CounterTable table{&value, &read_counter};
        table.payload.back() = std::byte{77};
        REQUIRE(provider.publish(temporary, &table));

        // Mutating and releasing Prepare inputs must not change the reserved contract or table.
        id = "changed";
        description[0] = std::byte{99};
        offsets[0] = 0;
        table.read = nullptr;
    }

    CHECK_FALSE(consumer.get("provider", contract()));

    // Activation makes the saved table available through ASCII case-insensitive provider references.
    owners.provider.state = PatchState::Installed;
    auto found = consumer.get("eXaMpLe.pRoViDeR", contract());

    REQUIRE(found);
    CHECK(reinterpret_cast<std::uintptr_t>(found->data()) % alignof(CounterTable) == 0);

    const auto table = copy_table(*found);
    CHECK(table.read(table.context) == 42);
    CHECK(table.payload.back() == std::byte{77});
}

TEST_CASE("Required Patch API retrieval rejects providers outside resolved dependencies", "[runtime][patch-api]") {
    Owners owners;
    auto &provider = *owners.provider.api;
    auto &consumer = *owners.consumer.api;
    auto &other = *owners.other.api;
    std::uint32_t value = 19;
    const CounterTable table{&value, &read_counter};

    REQUIRE(provider.publish(contract(), &table));
    REQUIRE(other.publish(contract(), &table));

    owners.provider.state = owners.other.state = PatchState::Installed;
    REQUIRE(consumer.get("Provider", contract()));

    auto denied = consumer.get("Other", contract());
    REQUIRE_FALSE(denied);
    CHECK(denied.error().owner == "Example.Consumer");
}

TEST_CASE("Patch API connection rejects null required functions", "[runtime][patch-api]") {
    Owners owners;
    auto &provider = *owners.provider.api;
    auto &consumer = *owners.consumer.api;
    std::uint32_t value = 1;
    const CounterTable missing{&value, nullptr};
    // Publication accepts the bytes; acquisition is where required callable pointers must be present.
    REQUIRE(provider.publish(contract("example.counter.v2"), &missing));

    owners.provider.state = PatchState::Installed;
    auto absent = consumer.get("provider", contract("example.counter.v2"));

    REQUIRE_FALSE(absent);
    CHECK(absent.error().owner == "Example.Consumer");
}

TEST_CASE("Patch API optional delivery waits for both owners and occurs once", "[runtime][patch-api]") {
    Owners owners;
    owners.consumer.providers.clear();
    auto &provider = *owners.provider.api;
    auto &consumer = *owners.consumer.api;
    Delivery delivery;
    std::vector<Diagnostic> diagnostics;

    {
        std::string provider_id = "PROVIDER";
        std::string api_id = "example.counter.v1";
        std::vector description(kDescription.begin(), kDescription.end());
        auto temporary = contract(api_id);
        temporary.description = description;
        REQUIRE(consumer.on_available(provider_id, temporary, &delivery, &available, owners.session));

        // The subscription retains the resolved provider identity and copied contract after these local inputs change.
        provider_id = "other";
        api_id = "different";
        description[0] = std::byte{99};
    }

    std::uint32_t value = 73;
    const CounterTable table{&value, &read_counter};
    REQUIRE(provider.publish(contract(), &table));

    // A preparing consumer waits for its own activation even when the provider has already activated.
    consumer.deliver_available(diagnostics);
    owners.provider.state = PatchState::Installed;
    consumer.deliver_available(diagnostics);

    CHECK(delivery.calls == 0);
    CHECK(owners.consumer.providers.empty());

    // Repeated delivery passes after consumer activation must leave one successful notification.
    owners.consumer.state = PatchState::Installed;
    consumer.deliver_available(diagnostics);
    consumer.deliver_available(diagnostics);

    CHECK(delivery.calls == 1);
    CHECK(delivery.value == 73);
    CHECK(diagnostics.empty());
}

TEST_CASE("Optional Patch API subscriptions report an incompatible late provider once", "[runtime][patch-api]") {
    Owners owners;
    auto &provider = *owners.provider.api;
    auto &consumer = *owners.consumer.api;
    Delivery mismatched, absent;
    REQUIRE(consumer.on_available("Missing", contract(), &absent, &available, owners.session));

    auto incompatible = contract();
    // Change comparison bytes while preserving ID and table size so only contract compatibility differs.
    auto description = kDescription;
    description[0] = std::byte{8};
    incompatible.description = description;
    REQUIRE(consumer.on_available("provider", incompatible, &mismatched, &available, owners.session));

    owners.consumer.state = PatchState::Installed;
    std::vector<Diagnostic> diagnostics;
    consumer.deliver_available(diagnostics);

    CHECK(diagnostics.empty());

    std::uint32_t value = 88;
    const CounterTable table{&value, &read_counter};
    REQUIRE(provider.publish(contract(), &table));
    owners.provider.state = PatchState::Installed;

    // A late incompatible provider produces one warning and never invokes the notification.
    consumer.deliver_available(diagnostics);
    consumer.deliver_available(diagnostics);

    CHECK(mismatched.calls == 0);
    CHECK(absent.calls == 0);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().owner == "Example.Consumer");
    CHECK(diagnostics.front().level == FC_LOG_WARNING);
    CHECK(owners.consumer.state == PatchState::Installed);
}

TEST_CASE("Patch API reservations stay unavailable after owner startup failure", "[runtime][patch-api]") {
    Owners owners;
    auto &provider = *owners.provider.api;
    auto &consumer = *owners.consumer.api;
    Delivery delivery;
    REQUIRE(consumer.on_available("provider", contract(), &delivery, &available, owners.session));

    std::uint32_t value = 12;
    const CounterTable table{&value, &read_counter};
    REQUIRE(provider.publish(contract(), &table));

    owners.provider.state = PatchState::Failed;
    CHECK_FALSE(consumer.get("provider", contract()));

    owners.consumer.state = PatchState::Failed;
    std::vector<Diagnostic> diagnostics;
    consumer.deliver_available(diagnostics);

    CHECK(delivery.calls == 0);

    // Failed preparation releases the copied unpublished table and optional subscription storage.
    provider.discard();
    consumer.discard();
}

TEST_CASE("Retained callbacks check activity before object access and run on the provider thread", "[runtime][patch-api]") {
    Owners owners;
    auto consumer = std::make_unique<Consumer>();
    fc::PrepareContext prepare{{&owners.consumer, &owners.runtime.host()}};

    const auto callback = prepare.callback<&Consumer::invoke>(*consumer);
    REQUIRE_FALSE(callback.try_call(10));
    CHECK(consumer->calls == 0);

    // Publish the initialized consumer before a provider thread attempts its retained callback.
    owners.consumer.state.store(PatchState::Installed, std::memory_order_release);
    std::thread::id provider_thread;
    bool returned{}, restored{};
    // Invoke on a separate provider thread; joining below keeps every borrowed capture alive until it returns.
    std::thread provider{[&] {
        provider_thread = std::this_thread::get_id();
        WSASetLastError(5678);
        SetLastError(5678);
        const auto before_windows = GetLastError();
        const auto before_socket = WSAGetLastError();

        // Compare both thread-local error values immediately around the call before other APIs can change them.
        auto result = callback.try_call(10);
        returned = result && *result == 17;
        const auto windows_error = GetLastError();
        const auto socket_error = WSAGetLastError();
        restored = windows_error == before_windows && socket_error == before_socket;
    }};
    provider.join();

    CHECK(returned);
    CHECK(restored);
    CHECK(consumer->thread == provider_thread);
    CHECK(consumer->origin == &owners.consumer);
    CHECK(fc::runtime::current_consumer() == nullptr);

    // Failed startup invalidates the retained binding before its handler storage is freed.
    owners.consumer.state.store(PatchState::Failed, std::memory_order_release);
    consumer.reset();
    REQUIRE_FALSE(callback.try_call(10));
}

TEST_CASE("Nested retained callbacks preserve the originating consumer and restore each surrounding error state", "[runtime][patch-api]") {
    Owners owners;
    Consumer consumer;
    fc::PrepareContext prepare{{&owners.consumer, &owners.runtime.host()}};
    const auto inner = prepare.callback(fc::bind<&Consumer::invoke>(consumer));
    owners.consumer.state = owners.provider.state = PatchState::Installed;

    // The outer callback enters as the provider; its nested consumer binding inherits that attribution.
    struct Provider {
        fc::ConsumerCallback<std::uint32_t(std::uint32_t) noexcept> inner;
        bool restored{};

        // Calls the inner consumer binding and checks restoration of this outer callback's error state.
        std::uint32_t invoke(std::uint32_t value) noexcept {
            WSASetLastError(111);
            SetLastError(111);
            const auto before_windows = GetLastError();
            const auto before_socket = WSAGetLastError();

            // Each nesting level restores the error state present at its own entry.
            auto result = inner.try_call(value);
            const auto windows_error = GetLastError();
            const auto socket_error = WSAGetLastError();
            restored = windows_error == before_windows && socket_error == before_socket;
            return result ? *result : 0;
        }
    } provider{inner};

    fc::PrepareContext provider_context{{&owners.provider, &owners.runtime.host()}};
    const auto outer = provider_context.callback<&Provider::invoke>(provider);

    WSASetLastError(333);
    SetLastError(333);
    const auto before_windows = GetLastError();
    const auto before_socket = WSAGetLastError();

    const auto result = outer.try_call(10);
    const auto windows_error = GetLastError();
    const auto socket_error = WSAGetLastError();

    REQUIRE(result == 17u);
    CHECK(provider.restored);
    CHECK(consumer.origin == &owners.provider);
    CHECK(windows_error == before_windows);
    CHECK(socket_error == before_socket);
    CHECK(fc::runtime::current_consumer() == nullptr);
}
