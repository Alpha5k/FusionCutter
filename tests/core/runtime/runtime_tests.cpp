#include "../../../src/core/runtime/core_runtime.hpp"
#include "../../../src/core/runtime/interface_router.hpp"
#include "../../../src/core/reporting/tracing.hpp"

#include <FusionCutter/SDK.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// The synthetic provider and consumer share this value with an exact layout to exercise copied transport.
struct CounterV1 {
    static constexpr std::string_view id = "CounterV1";
    std::int32_t value{};
};

static_assert(fc::InterfaceContract<CounterV1>);

// One state object exposes query, connection, and copied-value facts without relying on private router containers.
struct InterfaceFixture {
    FC_Bool query_result{FC_TRUE};
    CounterV1 supplied{42};
    std::uint32_t connections{};
    CounterV1 connected{};
};

// Records the largest interface query accepted by framework-owned scratch storage without retaining that storage.
struct InterfaceCapacityFixture {
    std::uint32_t calls{};
    std::uint32_t requested_size{};
};

// Captures routed framework diagnostics so interface ABI violations are tested as user-visible behavior.
struct LogCapture {
    std::vector<std::string> messages;

    [[nodiscard]] fc::CoreLogger logger() noexcept {
        return {this, "Interfaces",
                [](const void*, FC_LogLevel) noexcept {
                    return true;
                },
                [](void* context, std::string_view, FC_LogLevel, std::string_view message) noexcept {
                    try {
                        static_cast<LogCapture*>(context)->messages.emplace_back(message);
                    } catch (...) {}
                }};
    }

    [[nodiscard]] bool contains(std::string_view text) const noexcept {
        return std::ranges::any_of(messages, [&](const std::string& message) {
            return message.find(text) != std::string::npos;
        });
    }
};

// These callbacks model the flat native provider table and the SDK connection thunk retained by a planned route.
void FC_CALL destroy_patch(void*, FC_PatchHandle) noexcept {}

FC_Bool FC_CALL query_interface(void* context, FC_PatchHandle, FC_StringView id, std::uint32_t size,
                                void* output) noexcept {
    auto& fixture = *static_cast<InterfaceFixture*>(context);
    if (fixture.query_result != FC_TRUE) {
        return fixture.query_result;
    }
    const std::string_view requested{id.data, id.size};
    if (requested != CounterV1::id || size != sizeof(CounterV1) || output == nullptr) {
        return FC_FALSE;
    }
    std::memcpy(output, &fixture.supplied, sizeof(fixture.supplied));
    return FC_TRUE;
}

FC_Bool FC_CALL query_interface_capacity(void* context, FC_PatchHandle, FC_StringView, std::uint32_t size,
                                         void* output) noexcept {
    auto& fixture = *static_cast<InterfaceCapacityFixture*>(context);
    ++fixture.calls;
    fixture.requested_size = size;
    std::memset(output, 0x5a, size);
    return FC_TRUE;
}

void FC_CALL connect_interface(void* context, const void* value) noexcept {
    auto& fixture = *static_cast<InterfaceFixture*>(context);
    std::memcpy(&fixture.connected, value, sizeof(fixture.connected));
    ++fixture.connections;
}

[[nodiscard]] fc::planning::PatchInstance provider_instance(InterfaceFixture& fixture) {
    // Static lifetime of the callback table matches the plugin catalog while each test supplies its own opaque state.
    static FC_PatchCallbacks callbacks{
        .context = nullptr, .destroy = &destroy_patch, .query_interface = &query_interface};
    callbacks.context = &fixture;
    return {callbacks, reinterpret_cast<FC_PatchHandle>(&fixture)};
}

// Builds the copied operation record that validation of the patch plan retains for an optional typed binding.
[[nodiscard]] fc::planning::OperationRecord binding_operation(InterfaceFixture& fixture) {
    return {.index = 0,
            .payload = fc::planning::InterfaceBindingOperation{.provider_patch = "Provider",
                                                               .id = std::string{CounterV1::id},
                                                               .size = sizeof(CounterV1),
                                                               .context = &fixture,
                                                               .connect = &connect_interface}};
}

// The two owners distinguish semantic service calls from transparent observations retained by the provider.
struct ServiceOwner {
    [[nodiscard]] std::int32_t read(std::int32_t offset) const noexcept {
        return 40 + offset;
    }
};

struct ObservationOwner {
    void receive(std::uint32_t value) noexcept {
        received = value;
        // Ambient corruption verifies that only the observation bridge restores errors on the provider thread.
        SetLastError(ERROR_ACCESS_DENIED);
        WSASetLastError(WSAEACCES);
    }
    std::uint32_t received{};
};

// Crash publication needs one coherent target so retained image-relative blockers can become stable addresses.
[[nodiscard]] fc::targets::RecognizedTarget mapped_target() {
    std::vector<fc::targets::OwnedImage> images;
#if defined(_M_X64)
    auto bootstrap = std::vector<std::byte>(512, std::byte{});
    images.push_back(fc::targets::OwnedImage::mapped(
        {FC_IMAGE_BOOTSTRAP, "ClassicCollection_Bootstrap_66702CD7", 0, bootstrap.size()}, std::move(bootstrap), {}));
    constexpr std::string_view game_profile = "ClassicCollection_Game_66702CD2";
#else
    constexpr std::string_view game_profile = "SteamRetail_Game_59EDE353";
#endif
    auto game = std::vector<std::byte>(512, std::byte{});
    images.push_back(
        fc::targets::OwnedImage::mapped({FC_IMAGE_GAME, game_profile, 0, game.size()}, std::move(game), {}));
    auto target = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(images));
    REQUIRE(target.has_value());
    return std::move(*target);
}

} // namespace

TEST_CASE("Interface routing connects optional consumers once and delays direct lookup until full publication",
          "[runtime][interfaces]") {
    fc::runtime::InterfaceRouter router;
    router.reserve(2, 2);
    InterfaceFixture fixture;
    const std::array operations{binding_operation(fixture)};
    auto prepared = router.prepare(operations);
    REQUIRE(prepared.has_value());

    // Consumer activation parks the route first; provider publication fulfills it before direct lookup is visible.
    router.connect_consumer(fc::catalog::PatchIndex{1}, std::move(*prepared));
    CHECK(fixture.connections == 0);
    auto provider = provider_instance(fixture);
    router.publish_provider(fc::catalog::PatchIndex{0}, provider, "Provider");
    CHECK(fixture.connections == 1);
    CHECK(fixture.connected.value == 42);

    CounterV1 found{};
    CHECK(router.find_active("Provider", CounterV1::id, sizeof(found), &found) == FC_FALSE);
    router.mark_active(fc::catalog::PatchIndex{0});
    CHECK(router.find_active("Provider", CounterV1::id, sizeof(found), &found) == FC_TRUE);
    CHECK(found.value == 42);
    CHECK(fixture.connections == 1);
}

TEST_CASE("Interface routing rejects noncanonical provider Booleans without exposing scratch bytes",
          "[runtime][interfaces]") {
    fc::runtime::InterfaceRouter router;
    LogCapture logs;
    router.set_logger(logs.logger());
    router.reserve(1, 0);
    InterfaceFixture fixture{.query_result = 7};
    auto provider = provider_instance(fixture);
    router.publish_provider(fc::catalog::PatchIndex{0}, provider, "Provider");
    router.mark_active(fc::catalog::PatchIndex{0});

    CounterV1 output{99};
    CHECK(router.find_active("Provider", CounterV1::id, sizeof(output), &output) == FC_FALSE);
    CHECK(output.value == 0);
    CHECK(router.noncanonical_result_count() == 1);
    CHECK(logs.contains("noncanonical Boolean for interface 'CounterV1'"));
}

TEST_CASE("Interface routing accepts 512 copied bytes and rejects the next byte before provider entry",
          "[runtime][interfaces][capacity]") {
    fc::runtime::InterfaceRouter router;
    router.reserve(1, 0);
    InterfaceCapacityFixture fixture;
    const FC_PatchCallbacks callbacks{
        .context = &fixture, .destroy = &destroy_patch, .query_interface = &query_interface_capacity};
    fc::planning::PatchInstance provider{callbacks, reinterpret_cast<FC_PatchHandle>(&fixture)};
    router.publish_provider(fc::catalog::PatchIndex{0}, provider, "Provider");
    router.mark_active(fc::catalog::PatchIndex{0});

    std::array<std::byte, 512> accepted{};
    REQUIRE(router.find_active("Provider", "CapacityV1", static_cast<std::uint32_t>(accepted.size()),
                               accepted.data()) == FC_TRUE);
    CHECK(fixture.calls == 1);
    CHECK(fixture.requested_size == static_cast<std::uint32_t>(accepted.size()));
    CHECK(std::ranges::all_of(accepted, [](std::byte value) {
        return value == std::byte{0x5a};
    }));

    // The immediate excess must not call plugin code or touch consumer memory with an oversized scratch copy.
    std::array<std::byte, 513> rejected;
    rejected.fill(std::byte{0x33});
    CHECK(router.find_active("Provider", "CapacityV1", static_cast<std::uint32_t>(rejected.size()), rejected.data()) ==
          FC_FALSE);
    CHECK(fixture.calls == 1);
    CHECK(std::ranges::all_of(rejected, [](std::byte value) {
        return value == std::byte{0x33};
    }));
}

TEST_CASE("SDK interface callable helpers hide transport and preserve errors only for observations",
          "[runtime][interfaces][sdk]") {
    const ServiceOwner service;
    const auto function = fc::interface_function<&ServiceOwner::read>(service);
    STATIC_REQUIRE(std::is_standard_layout_v<decltype(function)>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<decltype(function)>);
    REQUIRE(function);
    CHECK(function(2) == 42);

    ObservationOwner consumer;
    const auto callback = fc::observation<&ObservationOwner::receive>(consumer);
    STATIC_REQUIRE(std::is_standard_layout_v<decltype(callback)>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<decltype(callback)>);
    SetLastError(ERROR_FILE_NOT_FOUND);
    callback(17);
    CHECK(consumer.received == 17);
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);

    WSASetLastError(WSAECONNRESET);
    callback(18);
    CHECK(WSAGetLastError() == WSAECONNRESET);
}

TEST_CASE("Trace channels remain inactive through the Prepare phase and use bounded accounting after arming",
          "[runtime][tracing]") {
    fc::runtime::TraceSession traces;
    fc::runtime::PreparedTraceChannel preparation;
    const std::string_view name = "Packets";
    const FC_TraceDefinition definition{.struct_size = sizeof(FC_TraceDefinition),
                                        .name = {name.data(), static_cast<std::uint32_t>(name.size())},
                                        .capacity = 2,
                                        .max_record_size = sizeof(std::uint32_t),
                                        .version = 1};
    // An abandoned token from the Prepare phase removes both channel identity and its aggregate budget reservation.
    FC_TraceHandle abandoned_handle{};
    {
        fc::runtime::PreparedTraceChannel abandoned;
        REQUIRE(traces.prepare_channel(fc::catalog::PatchIndex{3}, &definition, abandoned, &abandoned_handle) ==
                FC_TRACE_CREATED);
        CHECK(traces.channel_count() == 1);
    }
    CHECK(traces.channel_count() == 0);
    CHECK(traces.reserved_bytes() == 0);
    CHECK(traces.enabled(abandoned_handle) == FC_FALSE);
    CHECK(traces.try_write(abandoned_handle, {}) == FC_FALSE);

    FC_TraceHandle handle{};
    REQUIRE(traces.prepare_channel(fc::catalog::PatchIndex{3}, &definition, preparation, &handle) == FC_TRACE_CREATED);
    REQUIRE(handle != nullptr);
    CHECK(handle != abandoned_handle);
    CHECK(traces.enabled(abandoned_handle) == FC_FALSE);
    const std::uint32_t payload = 0x1234;
    const FC_ByteView bytes{reinterpret_cast<const std::uint8_t*>(&payload), sizeof(payload)};
    // A prepared producer cannot publish or accumulate drops before successful commit arms its patch.
    CHECK(traces.enabled(handle) == FC_FALSE);
    CHECK(traces.try_write(handle, bytes) == FC_FALSE);

    // Capacity counts pending records, so the third valid write is the one capacity drop until drain frees slots.
    traces.arm(preparation);
    CHECK(traces.enabled(handle) == FC_TRUE);
    CHECK(traces.try_write(handle, bytes) == FC_TRUE);
    CHECK(traces.try_write(handle, bytes) == FC_TRUE);
    CHECK(traces.try_write(handle, bytes) == FC_FALSE);
    FC_TraceHealth health{.struct_size = sizeof(FC_TraceHealth)};
    traces.health(handle, &health);
    CHECK(health.accepted == 2);
    CHECK(health.written == 0);
    CHECK(health.dropped == 1);

    // Invalid tokens and incomplete output prefixes are rejected without touching channel or output storage.
    const auto fabricated = reinterpret_cast<FC_TraceHandle>(std::numeric_limits<std::uintptr_t>::max());
    CHECK(traces.enabled(fabricated) == FC_FALSE);
    CHECK(traces.try_write(fabricated, bytes) == FC_FALSE);
    FC_TraceHealth incomplete{.struct_size = offsetof(FC_TraceHealth, output_failed), .accepted = 17};
    traces.health(handle, &incomplete);
    CHECK(incomplete.accepted == 17);
}

TEST_CASE("Trace aggregate storage accepts its largest single ring and rejects one additional slot",
          "[runtime][tracing][capacity]") {
    fc::runtime::TraceSession traces;
    constexpr std::string_view boundary_name = "BoundaryA";
    const auto can_prepare = [&](std::uint32_t capacity) {
        const FC_TraceDefinition definition{
            .struct_size = sizeof(FC_TraceDefinition),
            .name = {boundary_name.data(), static_cast<std::uint32_t>(boundary_name.size())},
            .capacity = capacity,
            .max_record_size = 1,
            .version = 1};
        fc::runtime::PreparedTraceChannel preparation;
        FC_TraceHandle handle{};
        return traces.prepare_channel(fc::catalog::PatchIndex{0}, &definition, preparation, &handle) ==
               FC_TRACE_CREATED;
    };

    // Search the architecture-specific storage layout instead of duplicating private channel-size arithmetic here.
    std::uint32_t accepted_capacity = 1;
    std::uint32_t rejected_capacity = 200'000;
    REQUIRE(can_prepare(accepted_capacity));
    REQUIRE_FALSE(can_prepare(rejected_capacity));
    while (accepted_capacity + 1 < rejected_capacity) {
        const auto candidate = accepted_capacity + (rejected_capacity - accepted_capacity) / 2;
        if (can_prepare(candidate)) {
            accepted_capacity = candidate;
        } else {
            rejected_capacity = candidate;
        }
    }
    REQUIRE(rejected_capacity == accepted_capacity + 1);

    const FC_TraceDefinition accepted{.struct_size = sizeof(FC_TraceDefinition),
                                      .name = {boundary_name.data(), static_cast<std::uint32_t>(boundary_name.size())},
                                      .capacity = accepted_capacity,
                                      .max_record_size = 1,
                                      .version = 1};
    fc::runtime::PreparedTraceChannel retained;
    FC_TraceHandle retained_handle{};
    REQUIRE(traces.prepare_channel(fc::catalog::PatchIndex{0}, &accepted, retained, &retained_handle) ==
            FC_TRACE_CREATED);
    const auto exact_reservation = traces.reserved_bytes();

    // A same-length name isolates the one-slot increase as the only additional aggregate storage charge.
    constexpr std::string_view beyond_name = "BoundaryB";
    const FC_TraceDefinition beyond{.struct_size = sizeof(FC_TraceDefinition),
                                    .name = {beyond_name.data(), static_cast<std::uint32_t>(beyond_name.size())},
                                    .capacity = rejected_capacity,
                                    .max_record_size = 1,
                                    .version = 1};
    fc::runtime::PreparedTraceChannel rejected;
    FC_TraceHandle rejected_handle{};
    CHECK(traces.prepare_channel(fc::catalog::PatchIndex{1}, &beyond, rejected, &rejected_handle) == FC_TRACE_REJECTED);
    CHECK(rejected_handle == nullptr);
    CHECK(traces.reserved_bytes() == exact_reservation);
}

TEST_CASE("Disabled tracing validates requests without reserving storage", "[runtime][tracing]") {
    fc::runtime::TraceSession traces{0};
    fc::runtime::PreparedTraceChannel preparation;
    const std::string_view name = "Disabled";
    const FC_TraceDefinition definition{.struct_size = sizeof(FC_TraceDefinition),
                                        .name = {name.data(), static_cast<std::uint32_t>(name.size())},
                                        .capacity = 1,
                                        .max_record_size = 8,
                                        .version = 1};
    FC_TraceHandle handle{};
    CHECK(traces.prepare_channel(fc::catalog::PatchIndex{0}, &definition, preparation, &handle) == FC_TRACE_DISABLED);
    CHECK(handle == nullptr);
    CHECK(traces.channel_count() == 0);
    CHECK(traces.reserved_bytes() == 0);
}

TEST_CASE("Concurrent trace producers remain bounded and account for every valid submission", "[runtime][tracing]") {
    fc::runtime::TraceSession traces;
    fc::runtime::PreparedTraceChannel preparation;
    const std::string_view name = "ConcurrentPackets";
    const FC_TraceDefinition definition{.struct_size = sizeof(FC_TraceDefinition),
                                        .name = {name.data(), static_cast<std::uint32_t>(name.size())},
                                        .capacity = 64,
                                        .max_record_size = sizeof(std::uint32_t),
                                        .version = 1};
    FC_TraceHandle handle{};
    REQUIRE(traces.prepare_channel(fc::catalog::PatchIndex{4}, &definition, preparation, &handle) == FC_TRACE_CREATED);
    traces.arm(preparation);

    // Eight game thread stand-ins race on one MPSC ring; every valid call becomes accepted or dropped exactly once.
    constexpr std::size_t producer_count = 8;
    constexpr std::size_t submissions_per_producer = 100;
    const std::uint32_t payload = 7;
    const FC_ByteView bytes{reinterpret_cast<const std::uint8_t*>(&payload), sizeof(payload)};
    std::array<std::thread, producer_count> producers;
    for (auto& producer : producers) {
        producer = std::thread([&] {
            for (std::size_t submission = 0; submission < submissions_per_producer; ++submission) {
                static_cast<void>(traces.try_write(handle, bytes));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    FC_TraceHealth health{.struct_size = sizeof(FC_TraceHealth)};
    traces.health(handle, &health);
    CHECK(health.accepted > 0);
    CHECK(health.accepted <= definition.capacity);
    CHECK(health.accepted + health.dropped == producer_count * submissions_per_producer);
    CHECK(health.written == 0);
}

TEST_CASE("Crash snapshots are append-only and fault state for guarded reads is thread-local", "[runtime][crash]") {
    fc::runtime::CrashPhaseCursors cursors;
    fc::runtime::CrashReporter crash{cursors};
    REQUIRE(crash.install());
    crash.set_core_phase(fc::runtime::CorePhase::Installation);
    crash.set_current_patch(fc::catalog::PatchIndex{9}, fc::planning::PatchPhase::Commit);

    // A finalized synthetic plugin catalog proves identity and callbacks are copied before patch plan cleanup.
    auto code_owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&destroy_patch));
    REQUIRE(code_owner.has_value());
    fc::catalog::PatchDefinitionRecord patch_definition;
    patch_definition.id = "CrashPatch";
    patch_definition.version = "2.0";
    patch_definition.selected_support = 0;
    patch_definition.supports.push_back(
        {.callbacks = {.destroy = &destroy_patch, .query_interface = &query_interface}});
    fc::catalog::PluginRecord plugin;
    plugin.code_owner = std::move(*code_owner);
    plugin.definition.id = "CrashPlugin";
    plugin.definition.version = "1.0";
    plugin.definition.patches.push_back(std::move(patch_definition));
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(plugin));
    fc::catalog::Catalog catalog{std::move(plugins)};
    crash.publish_catalog(catalog);
    const auto admitted = crash.snapshot();
    REQUIRE(admitted.annotations.size() == 3);
    CHECK(admitted.annotations[0].kind == fc::runtime::CrashAnnotationKind::Plugin);
    CHECK(std::strcmp(admitted.annotations[0].label, "CrashPlugin") == 0);
    CHECK(std::strcmp(admitted.annotations[0].detail, "1.0") == 0);
    CHECK(admitted.annotations[0].address != 0);
    CHECK(admitted.annotations[0].size != 0);
    CHECK(admitted.annotations[0].module[0] != '\0');
    CHECK(admitted.annotations[1].kind == fc::runtime::CrashAnnotationKind::Callback);
    CHECK(admitted.annotations[1].address == reinterpret_cast<std::uintptr_t>(&destroy_patch));
    CHECK(std::strcmp(admitted.annotations[2].detail, "QueryInterface") == 0);

    // Residual exposure publishes both the failed owner and each range future installation must treat as blocked.
    const std::array claims{fc::planning::MemoryClaim{
        .patch = {9}, .image = FC_IMAGE_GAME, .rva = 0x120, .size = 8, .access = fc::planning::ClaimAccess::Write}};
    auto target = mapped_target();
    const auto blocked_address = target.find(FC_IMAGE_GAME)->info().base + 0x120;
    fc::patching::NativePatchResources resources;
    fc::patching::HookPreparation hooks;
    crash.publish_retained_failure(fc::catalog::PatchIndex{9}, fc::patching::RollbackResult::Residual, claims,
                                   "RetainedPatch", target, resources, hooks);
    const auto snapshot = crash.snapshot();
    REQUIRE(snapshot.annotations.size() == 5);
    CHECK(snapshot.annotations[3].kind == fc::runtime::CrashAnnotationKind::RetainedFailure);
    CHECK(std::strcmp(snapshot.annotations[3].label, "RetainedPatch") == 0);
    CHECK(std::strcmp(snapshot.annotations[3].detail, "Residual rollback") == 0);
    CHECK(snapshot.annotations[4].address == blocked_address);
    CHECK(snapshot.annotations[4].size == 8);
    CHECK(snapshot.core_phase == fc::runtime::CorePhase::Installation);
    CHECK(snapshot.has_current_patch);
    CHECK(snapshot.current_patch == fc::catalog::PatchIndex{9});

    // The lexical marker suppresses only this thread's guarded read, never a simultaneous fault on another thread.
    constexpr std::uint32_t exception = 0xc0000005;
    std::atomic_bool other_thread_suppressed{true};
    {
        fc::runtime::CrashReporter::ExpectedFaultScope scope{exception};
        CHECK(fc::runtime::CrashReporter::expected_fault(exception));
        std::thread other([&] {
            other_thread_suppressed.store(fc::runtime::CrashReporter::expected_fault(exception),
                                          std::memory_order_release);
        });
        other.join();
    }
    CHECK_FALSE(other_thread_suppressed.load(std::memory_order_acquire));
    CHECK_FALSE(fc::runtime::CrashReporter::expected_fault(exception));
}

TEST_CASE("Loader initialization records enforce the complete tuple for ABI generation 1", "[runtime][core-api]") {
    FC_InitializeArgs arguments{.struct_size = sizeof(FC_InitializeArgs),
                                .host_role = FC_HOST_ROLE_CLIENT,
                                .loader_startup = {.loader_kind = FC_LOADER_KIND_DINPUT8,
                                                   .direct_input_chain = FC_DIRECT_INPUT_CHAIN_SYSTEM_ONLY}};
    auto copied = fc::runtime::copy_initialization_request(&arguments);
    REQUIRE(copied.has_value());
    CHECK(copied->role == FC_HOST_ROLE_CLIENT);
    CHECK(copied->loader.kind == fc::runtime::LoaderKind::DirectInput8);

    // The independently sized outer record accepts an unknown tail but never a missing known prefix.
    arguments.struct_size = sizeof(FC_InitializeArgs) + 16;
    CHECK(fc::runtime::copy_initialization_request(&arguments).has_value());
    arguments.struct_size = sizeof(FC_InitializeArgs) - 1;
    CHECK_FALSE(fc::runtime::copy_initialization_request(&arguments).has_value());
    arguments.struct_size = sizeof(FC_InitializeArgs);

    // Known scalar values still fail when they form a loader, role, and chain tuple no shipping entry can produce.
    arguments.loader_startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE;
    CHECK_FALSE(fc::runtime::copy_initialization_request(&arguments).has_value());
    arguments.loader_startup.loader_kind = FC_LOADER_KIND_BATTLEFRONT2;
    CHECK(fc::runtime::copy_initialization_request(&arguments).has_value());
    arguments.host_role = FC_HOST_ROLE_SERVER;
    CHECK_FALSE(fc::runtime::copy_initialization_request(&arguments).has_value());

    // A fixed child basename without an in-bounds terminator cannot be copied into framework-owned storage.
    arguments.host_role = FC_HOST_ROLE_CLIENT;
    std::memset(arguments.loader_startup.selected_proxy_basename, 'x',
                sizeof(arguments.loader_startup.selected_proxy_basename));
    CHECK_FALSE(fc::runtime::copy_initialization_request(&arguments).has_value());
}
