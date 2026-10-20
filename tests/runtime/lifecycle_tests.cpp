#include "patches/installation.hpp"
#include "core_runtime.hpp"
#include "native/hook_registry.hpp"
#include "patches/patch_api.hpp"
#include "patches/late_images.hpp"
#include "native/plan.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace fc::runtime;

// Retains callback results after a handler is destroyed so tests can inspect its complete lifecycle.
struct Observation {
    std::vector<std::string> calls;
    std::vector<std::string> *sequence{};

    // Adds a callback to this patch's history and, when supplied, the shared installation history.
    void record(std::string call) {
        calls.push_back(call);
        if (sequence) {
            sequence->push_back(std::move(call));
        }
    }

    bool ignore_rejected_read{}, fail_prepare{}, write{};
    std::uint32_t value{};
    // A nonzero address requests a function-pointer write instead of a numeric data write.
    std::uintptr_t function_target{};
    std::uint32_t rva{};
    // When supplied, Prepare changes this value before the installer rechecks the planned write.
    std::uint32_t *live_value{};
    bool explicit_evidence{};
    unsigned destroyed{};
    bool resources_at_destruction{};
    std::int64_t elapsed{};
};

// Borrows the fixture's observation and patch identity for the lifetime of its C handler.
struct Instance {
    Observation *observation;
    std::string id;
    PatchRecord *owner;
};

// Creates the lifecycle handler; its Observation and PatchRecord identity must outlive it.
std::uint32_t FC_CDECL create(void *context, void *identity, const FC_Host *, const FC_Value *, std::uint32_t, FC_Handler *out,
                              FC_ErrorSink) {
    auto &owner = *static_cast<PatchRecord *>(identity);
    auto *observed = static_cast<Observation *>(context);
    auto handler = std::make_unique<Instance>(
        observed, owner.internal_factory.create ? owner.resolved.owner : std::string{owner.definition->id.data, owner.definition->id.size},
        &owner);
    observed->record("create " + handler->id);

    // The runtime owns the released instance through destroy; every callback records its place in the lifecycle.
    *out = {};
    out->context = handler.release();
    out->destroy = [](void *raw) {
        // Observe resource lifetime before releasing the instance, including cleanup after rejected preparation.
        std::unique_ptr<Instance> instance{static_cast<Instance *>(raw)};
        ++instance->observation->destroyed;
        instance->observation->resources_at_destruction = instance->owner->plan && instance->owner->api;
        instance->observation->record("destroy " + instance->id);
    };

    // Submit real operations so conflict checks and function ownership checks run through the native planner.
    out->plan = [](void *raw, FC_Plan *plan, FC_ErrorSink error) -> std::uint32_t {
        auto &instance = *static_cast<Instance *>(raw);
        instance.observation->record("plan " + instance.id);
        if (instance.observation->ignore_rejected_read) {
            FC_ReadClaim claim{};
            claim.location = {{"fixture", 7}, UINT32_MAX, {"invalid read", 12}, 0};
            claim.size = 1;
            std::uint8_t value{};
            plan->read(plan->context, &claim, &value, error);
            return 1;
        }

        if (!instance.observation->write && !instance.observation->function_target) {
            return 1;
        }

        FC_Operation operation{};
        if (instance.observation->function_target) {
            operation.kind = FC_OPERATION_FIXUP;
            operation.value.fixup.location = {{"fixture", 7}, instance.observation->rva, {"function", 8}, 0};
            operation.value.fixup.target.kind = FC_ADDRESS_FUNCTION;
            operation.value.fixup.target.function = instance.observation->function_target;
            operation.value.fixup.width = sizeof(std::uintptr_t);
            return plan->submit(plan->context, &operation, nullptr, error);
        }

        operation.kind = FC_OPERATION_WRITE;
        operation.value.write = {{{"fixture", 7}, instance.observation->rva, {"value", 5}, 0},
                                 {},
                                 {reinterpret_cast<const std::uint8_t *>(&instance.observation->value), 4},
                                 4,
                                 0};
        const std::uint32_t initial{};
        if (instance.observation->explicit_evidence) {
            operation.value.write.evidence = {
                FC_EVIDENCE_EXACT_BYTES, {reinterpret_cast<const std::uint8_t *>(&initial), sizeof(initial)}, {}, {}};
        }

        return plan->submit(plan->context, &operation, nullptr, error);
    };

    // Inject a live-data change or recoverable failure after planning and before installation revalidation.
    out->prepare = [](void *raw, FC_ErrorSink error) -> std::uint32_t {
        auto &instance = *static_cast<Instance *>(raw);
        instance.observation->record("prepare " + instance.id);
        if (instance.observation->live_value) {
            *instance.observation->live_value = 7;
        }
        if (instance.observation->fail_prepare) {
            constexpr FC_Text message{"Deliberate preparation failure", 30};
            error.write(error.context, message, 123);
            return 0;
        }
        return 1;
    };

    out->activate = [](void *raw) {
        auto &instance = *static_cast<Instance *>(raw);
        instance.observation->record("activate " + instance.id);
    };

    out->update = [](void *raw, std::int64_t, std::int64_t elapsed) {
        auto &instance = *static_cast<Instance *>(raw);
        instance.observation->record("update " + instance.id);
        instance.observation->elapsed = elapsed;
    };

    return 1;
}

// Creates a runtime integration handler using the fixture's lifecycle callbacks and disables periodic updates.
std::uint32_t FC_CDECL create_internal(void *context, void *identity, const FC_Host *host, const FC_Value *settings, std::uint32_t count,
                                       FC_Handler *out, FC_ErrorSink error) {
    const auto result = create(context, identity, host, settings, count, out, error);
    out->update = nullptr;
    return result;
}

// Owns three patch declarations, callback histories and writable image storage used by the real installer.
struct Fixture {
    FC_Host host{sizeof(FC_Host), native_api(), patch_api_host()};
    HookRegistry hooks;
    FC_Plugin declaration{};
    std::array<FC_Patch, 3> definitions{};
    std::array<FC_Variant, 3> variants{};
    std::vector<std::string> sequence;
    std::array<Observation, 3> observations;
    std::array<std::string, 3> ids{"Zulu", "Alpha", "Middle"};
    std::array<std::string_view, 1> filenames{"fixture.dll"};
    ImageProfile profile{"fixture", filenames, 4096, {}};
    void *memory = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    LiveImageObservations images;
    SelectionSession session{{&profile, 1}, FC_ROLE_CLIENT};

    // Builds three Planning records with captured settings and a shared image, bypassing plugin admission.
    Fixture() {
        REQUIRE(memory);
        declaration.id = {"Example", 7};
        auto plugin = std::make_unique<PluginRecord>();
        plugin->admitted.definition = &declaration;
        plugin->configuration.emplace();

        images.push_back({"fixture.dll", LoadedImage{&profile,
                                                     "fixture.dll",
                                                     reinterpret_cast<std::uintptr_t>(memory),
                                                     4096,
                                                     {{".data", reinterpret_cast<std::uintptr_t>(memory), 4096,
                                                       IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE}}}});

        // Factories borrow observations and definitions; heap-owned patch records keep every identity pointer stable.
        for (std::size_t i = 0; i != definitions.size(); ++i) {
            observations[i].sequence = &sequence;
            auto &definition = definitions[i];
            definition.id = {ids[i].data(), static_cast<std::uint32_t>(ids[i].size())};
            definition.update_interval_ms = 50;
            variants[i].factory = {&observations[i], &create};
            auto patch = std::make_unique<PatchRecord>();
            patch->plugin = plugin.get();
            patch->definition = &definition;
            patch->resolved.owner = "Example." + ids[i];
            patch->state = PatchState::Planning;
            patch->enabled = patch->selected_for_startup = true;
            patch->selection = PatchSelection{SelectionState::Ready, &variants[i], i, {{"game", &profile, 0}}, {}};
            patch->api = std::make_unique<PatchApiReservations>(*patch);
            plugin->configuration->variants.push_back({&variants[i], {}});
            session.patches.push_back(std::move(patch));
        }

        session.plugins.push_back(std::move(plugin));
    }

    // Releases the synthetic image allocation after the test has inspected installation results.
    ~Fixture() {
        VirtualFree(memory, 0, MEM_RELEASE);
    }

    // Installs the ready patches and, when supplied, a runtime integration handler.
    void install(PatchRecord *internal = nullptr) {
        const std::array owners{internal};
        install_ready_patches(session, images, {&host, &hooks, nullptr, nullptr, {owners.data(), internal ? 1u : 0u}});
    }
};
} // namespace

TEST_CASE("Runtime integrations install first and retain memory claims for later patches", "[runtime][lifecycle]") {
    Fixture fixture;
    Observation observed;
    observed.sequence = &fixture.sequence;
    observed.function_target = reinterpret_cast<std::uintptr_t>(&create_internal);
    PatchRecord internal;
    internal.resolved.owner = "Fusion Cutter.Game";
    internal.internal_factory = {&observed, &create_internal};
    internal.state = PatchState::Planning;
    internal.selection.state = SelectionState::Ready;
    internal.selection.images.push_back({"game", &fixture.profile, 0});

    auto &public_patch = *fixture.session.patches[1];
    fixture.observations[1].write = true;
    public_patch.state = PatchState::Waiting;
    fixture.install(&internal);
    REQUIRE(internal.state == PatchState::Installed);
    CHECK(fixture.sequence.front() == "create Fusion Cutter.Game");
    CHECK(*static_cast<std::uintptr_t *>(fixture.memory) == observed.function_target);

    // A later public patch must respect the integration's installed memory claim.
    public_patch.state = PatchState::Planning;
    fixture.install(&internal);
    CHECK(public_patch.state == PatchState::Failed);
    CHECK(fixture.observations[1].destroyed == 1);
    CHECK(*static_cast<std::uintptr_t *>(fixture.memory) == observed.function_target);
}

TEST_CASE("Runtime integrations can detect images when no patch is waiting", "[runtime][lifecycle]") {
    std::array<wchar_t, 32768> path;
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    REQUIRE(size);
    const auto filename = std::filesystem::path{std::wstring_view{path.data(), size}}.filename().string();
    const std::array filenames{std::string_view{filename}};
    ImageProfile profile{"PrivateFixture", filenames, 1, {}};

    SECTION("A recognized image remains loaded at its existing observation index") {}
    SECTION("An already loaded incompatible image is reported once") {
        profile.image_size = UINT32_MAX;
    }

    SelectionSession session{{&profile, 1}, FC_ROLE_CLIENT};
    LiveImageObservations images;
    images.push_back({filename, std::nullopt});

    // The extra filename keeps detection active even though the patch list is empty.
    CHECK(process_awaited_images(session, images).empty());
    const auto changed = process_awaited_images(session, images, filenames);
    REQUIRE(changed == std::vector<std::size_t>{0});

    if (profile.image_size == 1) {
        REQUIRE(images[0].outcome);
        REQUIRE(*images[0].outcome);
        CHECK((*images[0].outcome)->profile == &profile);
    } else {
        CHECK_FALSE(images[0].outcome);
    }

    CHECK(process_awaited_images(session, images, filenames).empty());
}

TEST_CASE("Named late DLL discovery retains the original observation index", "[runtime][lifecycle]") {
    const std::filesystem::path path{FC_FIXTURE_NO_EXPORT};
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    REQUIRE(input);
    std::vector<std::byte> bytes(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    REQUIRE(input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    const auto layout = parse_file_image(bytes, "late DLL fixture");
    REQUIRE(layout);
    std::vector<SectionProfile> sections;
    for (const auto &section : layout->sections) {
        sections.push_back({section.name, section.rva, section.virtual_size});
    }
    const auto filename = path.filename().string();
    const std::array filenames{std::string_view{filename}};
    const std::array catalog{ImageProfile{"LateFixture", filenames, layout->image_size, sections}};
    SelectionSession session{catalog, FC_ROLE_CLIENT};
    LiveImageObservations images{{filename, probe_loaded_image(filename, catalog)}};
    REQUIRE(images[0].outcome);
    REQUIRE_FALSE(*images[0].outcome);
    CHECK(process_awaited_images(session, images, filenames).empty());

    const auto module = LoadLibraryW(path.c_str());
    REQUIRE(module);
    const auto changed = process_awaited_images(session, images, filenames);

    REQUIRE(changed == std::vector<std::size_t>{0});
    REQUIRE(images[0].outcome);
    REQUIRE(*images[0].outcome);
    CHECK((*images[0].outcome)->base == reinterpret_cast<std::uintptr_t>(module));
    CHECK(process_awaited_images(session, images, filenames).empty());
    FreeLibrary(module);
}

TEST_CASE("Dependencies install before consumers; unrelated patches use ASCII case-insensitive ID order", "[runtime][lifecycle]") {
    Fixture fixture;
    auto &provider = *fixture.session.patches[0];
    auto &consumer = *fixture.session.patches[1];
    consumer.providers.push_back(&provider);

    const auto ordered = ready_patch_order(fixture.session);

    REQUIRE(ordered.size() == 3);
    CHECK(ordered[0] == fixture.session.patches[2].get());
    CHECK(ordered[1] == &provider);
    CHECK(ordered[2] == &consumer);

    fixture.install();
    for (const auto &patch : fixture.session.patches) {
        CHECK(patch->state == PatchState::Installed);
    }

    // The batch collects every plan first, then activates each provider before preparing its consumer.
    CHECK(fixture.sequence == std::vector<std::string>{"create Middle", "plan Middle", "create Zulu", "plan Zulu", "create Alpha",
                                                       "plan Alpha", "prepare Middle", "activate Middle", "prepare Zulu", "activate Zulu",
                                                       "prepare Alpha", "activate Alpha"});
}

TEST_CASE("An ignored rejected Plan request fails dependents before preparation", "[runtime][lifecycle]") {
    Fixture fixture;
    auto &provider = *fixture.session.patches[0];
    auto &consumer = *fixture.session.patches[1];
    consumer.providers.push_back(&provider);
    fixture.observations[0].ignore_rejected_read = true;

    fixture.install();
    CHECK(provider.state == PatchState::Failed);
    CHECK_FALSE(provider.handler.context);
    CHECK_FALSE(provider.plan);
    CHECK(fixture.observations[0].destroyed == 1);
    CHECK(consumer.state == PatchState::Failed);
    CHECK(fixture.observations[1].calls.empty());
    CHECK(fixture.session.patches[2]->state == PatchState::Installed);
    CHECK(*static_cast<std::uint32_t *>(fixture.memory) == 0);
}

TEST_CASE("A provider's preparation failure rejects its consumer while an independent patch installs", "[runtime][lifecycle]") {
    Fixture fixture;
    // Selected Continue variants override Fatal defaults for both the failed provider and its rejected consumer.
    fixture.definitions[0].failure_policy = fixture.definitions[1].failure_policy = FC_FAILURE_FATAL;
    fixture.session.patches[1]->providers.push_back(fixture.session.patches[0].get());
    fixture.observations[0].fail_prepare = true;

    fixture.install();

    CHECK(fixture.session.patches[0]->state == PatchState::Failed);
    CHECK(fixture.session.patches[1]->state == PatchState::Failed);
    REQUIRE(fixture.session.diagnostics.size() == 2);
    CHECK(fixture.session.diagnostics[0].owner == "Example.Zulu");
    CHECK(fixture.session.diagnostics[0].windows_error == 123);
    CHECK(fixture.session.diagnostics[0].level == FC_LOG_ERROR);
    CHECK(fixture.session.diagnostics[1].owner == "Example.Alpha");
    CHECK(fixture.session.patches[2]->state == PatchState::Installed);
    CHECK(fixture.observations[1].calls == std::vector<std::string>{"create Alpha", "plan Alpha", "destroy Alpha"});
    CHECK(fixture.observations[0].resources_at_destruction);
    CHECK_FALSE(fixture.session.patches[0]->handler.context);
    CHECK_FALSE(fixture.session.patches[0]->plan);
    CHECK_FALSE(fixture.session.fatal);
}

TEST_CASE("Selected Fatal variant preparation failure enters the production fatal boundary", "[.variant-prepare-child]") {
    Fixture fixture;
    fixture.variants[1].failure_policy = FC_FAILURE_FATAL;
    fixture.observations[1].fail_prepare = true;
    fixture.install();
    FAIL("Fatal variant preparation failure returned");
}

TEST_CASE("A selected Fatal variant preparation failure stops the child process", "[runtime][lifecycle]") {
    std::array<wchar_t, 32768> path{};
    REQUIRE(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    auto command = L"\"" + std::wstring{path.data()} + L"\" \"[.variant-prepare-child]\"";
    CHECK(fc::tests::run_child(std::move(command), nullptr, 10000, CREATE_NO_WINDOW) == 0xd1);
}

TEST_CASE("Delayed updates for installed patches run once with the actual elapsed time", "[runtime][lifecycle]") {
    Fixture fixture;
    fixture.install();
    auto &patch = *fixture.session.patches[0];
    const auto activated = patch.last_update;
    const auto before = fixture.observations[0].calls.size();

    run_due_updates(fixture.session, activated + std::chrono::milliseconds{49});

    CHECK(fixture.observations[0].calls.size() == before);

    run_due_updates(fixture.session, activated + std::chrono::milliseconds{250});

    CHECK(fixture.observations[0].calls.size() == before + 1);
    CHECK(fixture.observations[0].elapsed == 250'000'000);

    run_due_updates(fixture.session, activated + std::chrono::milliseconds{250});

    CHECK(fixture.observations[0].calls.size() == before + 1);
    CHECK(patch.next_update == activated + std::chrono::milliseconds{300});
}

TEST_CASE("A data write permits changes during prepare() unless explicit evidence rejects them", "[runtime][lifecycle]") {
    for (const bool explicit_evidence : {false, true}) {
        Fixture fixture;
        auto &observation = fixture.observations[1];
        observation.write = true;
        observation.value = 22;
        observation.live_value = static_cast<std::uint32_t *>(fixture.memory);
        observation.explicit_evidence = explicit_evidence;

        fixture.install();

        CHECK(fixture.session.patches[1]->state == (explicit_evidence ? PatchState::Failed : PatchState::Installed));
        CHECK(*observation.live_value == (explicit_evidence ? 7u : 22u));
        if (explicit_evidence) {
            CHECK(fixture.observations[1].destroyed == 1);
        }
    }
}
