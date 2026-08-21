#include "../../../src/core/runtime/late_images.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

// Each script isolates a late pass that installs, collides with an installed claim, or violates live image safety.
enum class PlanScript {
    Empty,
    ReadClaim,
    SafeHook,
    UnsafeWrite,
};

// Callback counters prove absence/rejection stops before the Create callback and arrival uses the common lifecycle.
struct PatchFixture {
    PlanScript script{PlanScript::Empty};
    std::size_t creates{};
    std::size_t plans{};
    std::size_t prepares{};
    std::size_t activates{};
    std::size_t updates{};
    std::size_t destroys{};
    std::uintptr_t original{};
};

// Converts fixture-owned bytes to the borrowed C view copied synchronously by the plan sink.
[[nodiscard]] FC_ByteView bytes(std::span<const std::byte> value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), static_cast<std::uint32_t>(value.size())};
}

// These native callbacks expose each resumed lifecycle boundary through the retained fixture counters above.
FC_CallStatus FC_CALL create_patch(void* context, const FC_CreateContext*, const FC_SettingsView*, const FC_ErrorSink*,
                                   FC_PatchHandle* output) {
    auto& fixture = *static_cast<PatchFixture*>(context);
    ++fixture.creates;
    *output = &fixture;
    return FC_CALL_OK;
}

void FC_CALL destroy_patch(void* context, FC_PatchHandle patch) {
    auto& fixture = *static_cast<PatchFixture*>(context);
    if (patch == &fixture) {
        ++fixture.destroys;
    }
}

// The hook conflict test never invokes its builder, but admission still requires valid callback code.
FC_CallStatus FC_CALL build_hook(const FC_HookBuildInput*, const FC_ErrorSink*) {
    return FC_CALL_OK;
}

// This non-null address gives the copied hook record an owner callback without requiring the hook to execute.
void FC_CALL hook_callback() {}

// Original binding would be observable if a regression allowed the fixture to reach native preparation.
void FC_CALL bind_original(void* context, std::uintptr_t original) {
    static_cast<PatchFixture*>(context)->original = original;
}

// Submits the fixture's operation shape while leaving other behavior in the Plan callback unchanged.
FC_CallStatus FC_CALL plan_patch(void*, FC_PatchHandle patch, const FC_PlanContext*, const FC_PlanSink* sink,
                                 const FC_ErrorSink*) {
    auto& fixture = *static_cast<PatchFixture*>(patch);
    ++fixture.plans;
    if (fixture.script == PlanScript::Empty) {
        return FC_CALL_OK;
    }
    if (fixture.script == PlanScript::ReadClaim) {
        // A read-only requirement is safe on a live image but still conflicts with an installed exclusive writer.
        std::uintptr_t resolved{};
        const FC_RequireRequest request{.struct_size = sizeof(FC_RequireRequest),
                                        .location = {.kind = FC_LOCATION_DATA, .rva = 64},
                                        .size = 4,
                                        .alignment = 4,
                                        .writable = FC_FALSE};
        return sink->require(sink->context, &request, &resolved) == FC_SUBMIT_ACCEPTED && resolved != 0
                   ? FC_CALL_OK
                   : FC_CALL_FAILED;
    }
    if (fixture.script == PlanScript::SafeHook) {
        // Function entry publication may mutate a live module because SafetyHook traps affected threads.
        const FC_NativeStorage no_storage{FC_NATIVE_STORAGE_NONE, FC_REGISTER_NONE, 0};
        const FC_NativeCall native_call{.struct_size = sizeof(FC_NativeCall),
                                        .result = {FC_NATIVE_VOID, 0, 0},
                                        .return_storage = no_storage,
                                        .cleanup = FC_STACK_CLEANUP_NONE};
        const FC_HookRequest request{.struct_size = sizeof(FC_HookRequest),
                                     .location = {.kind = FC_LOCATION_FUNCTION, .rva = 96},
                                     .kind = FC_HOOK_FUNCTION_ENTRY,
                                     .native_call = &native_call,
                                     .builder = {.build = &build_hook, .entry_size = 1},
                                     .context = &fixture,
                                     .callback = reinterpret_cast<std::uintptr_t>(&hook_callback),
                                     .original_context = &fixture,
                                     .bind_original = &bind_original};
        return sink->hook(sink->context, &request) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
    }

    // An ordinary byte write is structurally valid but lacks the suspended-thread guarantee required by GalaxyPeer.
    const std::array replacement{std::byte{0x44}};
    const FC_WriteRequest request{.struct_size = sizeof(FC_WriteRequest),
                                  .location = {.kind = FC_LOCATION_DATA, .rva = 80},
                                  .kind = FC_WRITE_BYTES,
                                  .bytes = bytes(replacement)};
    return sink->write(sink->context, &request) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
}

// Successful empty plans use these callbacks to prove the common installer reaches normal phase publication.
FC_CallStatus FC_CALL prepare_patch(void*, FC_PatchHandle patch, const FC_PrepareContext*, const FC_ErrorSink*) {
    ++static_cast<PatchFixture*>(patch)->prepares;
    return FC_CALL_OK;
}

void FC_CALL activate_patch(void*, FC_PatchHandle patch, const FC_ActivateContext*) {
    ++static_cast<PatchFixture*>(patch)->activates;
}

void FC_CALL update_patch(void*, FC_PatchHandle patch, const FC_UpdateContext*) noexcept {
    ++static_cast<PatchFixture*>(patch)->updates;
}

// Wraps one fixture in a selected support; direct `Catalog` construction keeps this test on runtime behavior.
[[nodiscard]] fc::catalog::PatchDefinitionRecord patch(std::string id, PatchFixture& fixture, FC_TargetImage image) {
    fc::catalog::SupportDefinitionRecord support{.layouts = FC_LAYOUT_GOG_RETAIL,
                                                 .roles = FC_HOST_ROLE_SERVER,
                                                 .image = image,
                                                 .callbacks = {.context = &fixture,
                                                               .create = &create_patch,
                                                               .destroy = &destroy_patch,
                                                               .plan = &plan_patch,
                                                               .prepare = &prepare_patch,
                                                               .activate = &activate_patch,
                                                               .update = &update_patch},
                                                 .failure_policy = FC_FAILURE_INHERIT};
    return {.id = std::move(id),
            .name = "Late runtime fixture",
            .enabled = FC_TRUE,
            .failure_policy = FC_FAILURE_CONTINUE,
            .supports = {std::move(support)},
            .selected_support = 0};
}

// Builds the immutable selected catalog consumed by PatchRuntimeState, with an optional preinstalled baseline patch.
[[nodiscard]] fc::catalog::Catalog catalog(PatchFixture& late, PatchFixture* baseline) {
    auto owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&plan_patch));
    REQUIRE(owner.has_value());
    fc::catalog::PluginRecord plugin;
    plugin.sdk_revision = FC_SDK_REVISION;
    plugin.code_owner = std::move(*owner);
    plugin.definition.id = "LateRuntimeFixture";
    if (baseline != nullptr) {
        plugin.definition.patches.push_back(patch("StartupBaseline", *baseline, FC_IMAGE_GAME));
    }
    plugin.definition.patches.push_back(patch("LatePatch", late, FC_IMAGE_GALAXY_PEER));
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(plugin));
    return fc::catalog::Catalog{std::move(plugins)};
}

// The startup target intentionally has no GalaxyPeer slot; arrival must fill it without moving this Game owner.
[[nodiscard]] fc::targets::RecognizedTarget gog_server_target() {
    auto bytes = std::vector<std::byte>(256, std::byte{});
    std::vector<fc::targets::OwnedImage> images;
    images.push_back(fc::targets::OwnedImage::mapped({FC_IMAGE_GAME, "GOGRetail_Game_59EDF52B", 0, bytes.size()},
                                                     std::move(bytes), {{0, 256}}, {{0, 256}}));
    auto target = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_SERVER, std::move(images));
    REQUIRE(target.has_value());
    return std::move(*target);
}

// Supplies a small owned mapping with the reviewed identity and access policy needed by late common validation.
[[nodiscard]] fc::targets::OwnedImage galaxy_peer_image() {
    auto bytes = std::vector<std::byte>(256, std::byte{});
    // Both reviewed Galaxy builds expose this exact GetExternalId prologue; the test needs only its decode shape.
    constexpr std::array prologue{std::byte{0x55}, std::byte{0x8b}, std::byte{0xec}, std::byte{0x83}, std::byte{0xec},
                                  std::byte{0x14}, std::byte{0x57}, std::byte{0x8b}, std::byte{0xf9}, std::byte{0xc3}};
    std::copy(prologue.begin(), prologue.end(), bytes.begin() + 96);
    return fc::targets::OwnedImage::mapped({FC_IMAGE_GALAXY_PEER, "GOGRetail_GalaxyPeer_59E6304A", 0, bytes.size()},
                                           std::move(bytes), {{0, 256}}, {{0, 256}}, {{0, 256}});
}

// Creates state after startup for the late image owner: the startup target is fixed and the late patch waits.
[[nodiscard]] std::unique_ptr<fc::runtime::PatchRuntimeState> runtime(PatchFixture& late,
                                                                      PatchFixture* baseline = nullptr) {
    auto result = std::make_unique<fc::runtime::PatchRuntimeState>(gog_server_target(), catalog(late, baseline));
    if (baseline != nullptr) {
        result->patches.record(*result->catalog.find_patch("StartupBaseline")).state =
            fc::planning::PatchState::Installed;
    }
    result->patches.record(*result->catalog.find_patch("LatePatch")).state = fc::planning::PatchState::WaitingForImage;
    result->awaited_images.reset_from_waiting(result->catalog, result->patches);
    return result;
}

// Probe scripts isolate absence, successful recognition, and permanent rejection at the image-detection boundary.
enum class ProbeScript {
    Absent,
    Recognized,
    Rejected,
};

// The injected result controls only detection; the production owner still attaches, validates, installs, and prunes.
struct ProbeFixture {
    ProbeScript script{};
    std::size_t calls{};
};

[[nodiscard]] fc::targets::LateProbeResult probe_image(void* context, const fc::targets::RecognizedTarget&,
                                                       FC_TargetImage image) {
    auto& fixture = *static_cast<ProbeFixture*>(context);
    ++fixture.calls;
    REQUIRE(image == FC_IMAGE_GALAXY_PEER);
    if (fixture.script == ProbeScript::Absent) {
        return std::optional<fc::targets::OwnedImage>{};
    }
    if (fixture.script == ProbeScript::Rejected) {
        return std::unexpected(
            fc::targets::LateProbeError{"Injected image identity mismatch", "Recognize GalaxyPeer image"});
    }
    return std::optional<fc::targets::OwnedImage>{galaxy_peer_image()};
}

// Retains component, severity, and message so pump tests enforce useful logs without relying on filesystem timing.
struct LogCapture {
    struct Record {
        std::string scope;
        FC_LogLevel level{};
        std::string message;
    };

    [[nodiscard]] fc::CoreLogger logger(std::string_view scope) noexcept {
        return {this, scope,
                [](const void*, FC_LogLevel level) noexcept {
                    return level >= FC_LOG_ERROR && level <= FC_LOG_DEBUG;
                },
                [](void* context, std::string_view scope, FC_LogLevel level, std::string_view message) noexcept {
                    auto& capture = *static_cast<LogCapture*>(context);
                    try {
                        capture.records.push_back({std::string{scope}, level, std::string{message}});
                    } catch (...) {
                        capture.allocation_failed = true;
                    }
                }};
    }

    [[nodiscard]] bool contains(std::string_view scope, FC_LogLevel level, std::string_view text) const {
        return std::ranges::any_of(records, [&](const auto& record) {
            return record.scope == scope && record.level == level && record.message.contains(text);
        });
    }

    std::vector<Record> records;
    bool allocation_failed{};
};

// Each run owns the diagnostic services that the common production installer expects to publish into.
[[nodiscard]] fc::runtime::LateImageResult run_late_pass(fc::runtime::PatchRuntimeState& runtime, ProbeFixture& probe,
                                                         LogCapture* logs = nullptr) {
    fc::runtime::TraceSession traces{0};
    fc::runtime::CrashPhaseCursors cursors;
    fc::runtime::CrashReporter crash{cursors};
    REQUIRE(crash.install());
    return fc::runtime::process_awaited_images(
        runtime, traces, crash,
        {.probe_context = &probe,
         .probe = &probe_image,
         .installation = {.logger = logs == nullptr ? fc::CoreLogger{} : logs->logger("Installation")},
         .logger = logs == nullptr ? fc::CoreLogger{} : logs->logger("LateImages"),
         .planning_logger = logs == nullptr ? fc::CoreLogger{} : logs->logger("Validation")});
}

// Returns the sole waiting fixture record without exposing its plugin catalog order to each assertion group.
[[nodiscard]] const fc::planning::PatchWorkRecord& late_record(const fc::runtime::PatchRuntimeState& runtime) {
    return runtime.patches.record(*runtime.catalog.find_patch("LatePatch"));
}

} // namespace

TEST_CASE("An absent reviewed image stays awaited without allocating patch state", "[runtime][late-image]") {
    PatchFixture patch_fixture;
    auto current = runtime(patch_fixture);
    ProbeFixture probe{ProbeScript::Absent};
    LogCapture logs;

    CHECK(run_late_pass(*current, probe, &logs) == fc::runtime::LateImageResult::Unchanged);
    CHECK(run_late_pass(*current, probe, &logs) == fc::runtime::LateImageResult::Unchanged);
    CHECK(probe.calls == 2);
    CHECK(current->awaited_images.contains(FC_IMAGE_GALAXY_PEER));
    CHECK(late_record(*current).state == fc::planning::PatchState::WaitingForImage);
    CHECK(patch_fixture.creates == 0);
    CHECK(current->target.find(FC_IMAGE_GALAXY_PEER) == nullptr);
    // Expected absence is polled, not diagnosed; repeated Update callbacks must not flood ordinary logs.
    CHECK(logs.records.empty());
}

TEST_CASE("A recognized image fills its stable slot and resumes through the common installer",
          "[runtime][late-image]") {
    PatchFixture patch_fixture;
    auto current = runtime(patch_fixture);
    const auto* stable_game_view = current->target.find(FC_IMAGE_GAME);
    REQUIRE(stable_game_view != nullptr);
    const auto stable_game_base = stable_game_view->info().base;
    ProbeFixture probe{ProbeScript::Recognized};
    LogCapture logs;

    CHECK(run_late_pass(*current, probe, &logs) == fc::runtime::LateImageResult::Changed);
    CHECK_FALSE(current->awaited_images.contains(FC_IMAGE_GALAXY_PEER));
    CHECK(current->target.find(FC_IMAGE_GAME) == stable_game_view);
    CHECK(current->target.find(FC_IMAGE_GAME)->info().base == stable_game_base);
    CHECK(current->target.find(FC_IMAGE_GALAXY_PEER) != nullptr);
    CHECK_FALSE(logs.allocation_failed);
    CHECK(logs.contains("LateImages", FC_LOG_INFO, "Recognized and retained late target image 'GalaxyPeer'"));
#if defined(_M_IX86)
    // GalaxyPeer is an x86 target, so its native build must complete Prepare and Activate callbacks before publication.
    CHECK(late_record(*current).state == fc::planning::PatchState::Installed);
    CHECK(patch_fixture.creates == 1);
    CHECK(patch_fixture.plans == 1);
    CHECK(patch_fixture.prepares == 1);
    CHECK(patch_fixture.activates == 1);
    REQUIRE(current->update_order.size() == 1);
    CHECK(logs.contains("Installation", FC_LOG_DEBUG, "Committing native changes for patch 'LatePatch'"));
    CHECK(logs.contains("Installation", FC_LOG_DEBUG, "Activating and publishing patch 'LatePatch'"));
    CHECK(logs.contains("Installation", FC_LOG_INFO, "Installed patch 'LatePatch'"));
#else
    // The x64 contract build still proves resume reaches the common installer, whose architecture guard rejects the
    // deliberately x86 fixture before native preparation.
    REQUIRE(late_record(*current).reason.has_value());
    CHECK(late_record(*current).state == fc::planning::PatchState::Failed);
    CHECK(late_record(*current).reason->operation == "Prepare native target");
    CHECK(patch_fixture.creates == 1);
    CHECK(patch_fixture.plans == 1);
    CHECK(patch_fixture.prepares == 0);
    CHECK(patch_fixture.activates == 0);
    CHECK(current->update_order.empty());
    CHECK(logs.contains("Installation", FC_LOG_ERROR, "Patch 'LatePatch' failed"));
#endif

    // Removal from the awaited mask makes every later pump call inert for this image, regardless of patch outcome.
    CHECK(run_late_pass(*current, probe, &logs) == fc::runtime::LateImageResult::Unchanged);
    CHECK(probe.calls == 1);
    current.reset();
    CHECK(patch_fixture.destroys == 1);
}

TEST_CASE("A permanent probe rejection skips waiting patches once with its focused recognition reason",
          "[runtime][late-image]") {
    PatchFixture patch_fixture;
    auto current = runtime(patch_fixture);
    ProbeFixture probe{ProbeScript::Rejected};
    LogCapture logs;

    CHECK(run_late_pass(*current, probe, &logs) == fc::runtime::LateImageResult::Changed);
    const auto& record = late_record(*current);
    REQUIRE(record.reason.has_value());
    CHECK(record.state == fc::planning::PatchState::Skipped);
    CHECK(record.reason->phase == fc::planning::PatchPhase::Selection);
    CHECK(record.reason->operation == "Recognize GalaxyPeer image");
    CHECK(record.reason->message == "Injected image identity mismatch");
    CHECK_FALSE(current->awaited_images.contains(FC_IMAGE_GALAXY_PEER));
    CHECK(patch_fixture.creates == 0);
    CHECK_FALSE(logs.allocation_failed);
    CHECK(logs.contains("LateImages", FC_LOG_WARNING, "Late target image 'GalaxyPeer' was permanently rejected"));

    CHECK(run_late_pass(*current, probe, &logs) == fc::runtime::LateImageResult::Unchanged);
    CHECK(probe.calls == 1);
}

TEST_CASE("Late validation preserves installed results and checks their immutable claim baseline",
          "[runtime][late-image]") {
    PatchFixture baseline;
    PatchFixture late{PlanScript::ReadClaim};
    auto current = runtime(late, &baseline);
    const auto baseline_index = *current->catalog.find_patch("StartupBaseline");
    current->installed_claims.push_back({.patch = baseline_index,
                                         .image = FC_IMAGE_GALAXY_PEER,
                                         .rva = 64,
                                         .size = 4,
                                         .access = fc::planning::ClaimAccess::Write});
    ProbeFixture probe{ProbeScript::Recognized};

    CHECK(run_late_pass(*current, probe) == fc::runtime::LateImageResult::Changed);
    CHECK(current->patches.record(baseline_index).state == fc::planning::PatchState::Installed);
    const auto& record = late_record(*current);
    REQUIRE(record.reason.has_value());
    CHECK(record.state == fc::planning::PatchState::Failed);
    CHECK(record.reason->phase == fc::planning::PatchPhase::Validation);
    CHECK(record.reason->operation == "Validate memory claims");
    CHECK(late.creates == 1);
    CHECK(late.plans == 1);
    CHECK(late.prepares == 0);
    CHECK(late.destroys == 1);
    CHECK(current->target.find(FC_IMAGE_GALAXY_PEER) != nullptr);
}

TEST_CASE("GalaxyPeer rejects raw late writes that cannot be published while other threads are suspended",
          "[runtime][late-image][safety]") {
    PatchFixture patch_fixture{PlanScript::UnsafeWrite};
    auto current = runtime(patch_fixture);
    ProbeFixture probe{ProbeScript::Recognized};

    CHECK(run_late_pass(*current, probe) == fc::runtime::LateImageResult::Changed);
    const auto& record = late_record(*current);
    REQUIRE(record.reason.has_value());
    CHECK(record.state == fc::planning::PatchState::Failed);
    CHECK(record.reason->phase == fc::planning::PatchPhase::Validation);
    CHECK(record.reason->operation == "Validate mutation safety for a late image");
    CHECK(patch_fixture.creates == 1);
    CHECK(patch_fixture.plans == 1);
    CHECK(patch_fixture.prepares == 0);
    CHECK(patch_fixture.destroys == 1);
}

TEST_CASE("GalaxyPeer admits a suspended function hook before applying ordinary installed conflicts",
          "[runtime][late-image][safety]") {
    PatchFixture baseline;
    PatchFixture late{PlanScript::SafeHook};
    auto current = runtime(late, &baseline);
    const auto baseline_index = *current->catalog.find_patch("StartupBaseline");
    current->installed_claims.push_back({.patch = baseline_index,
                                         .image = FC_IMAGE_GALAXY_PEER,
                                         .rva = 96,
                                         .size = 16,
                                         .access = fc::planning::ClaimAccess::Write});
    ProbeFixture probe{ProbeScript::Recognized};

    CHECK(run_late_pass(*current, probe) == fc::runtime::LateImageResult::Changed);
    const auto& record = late_record(*current);
    REQUIRE(record.reason.has_value());
    CHECK(record.state == fc::planning::PatchState::Failed);
    // Reaching claim validation proves the profile policy accepted the function entry operation managed by SafetyHook.
    CHECK(record.reason->operation == "Validate memory claims");
    CHECK(late.creates == 1);
    CHECK(late.plans == 1);
    CHECK(late.prepares == 0);
    CHECK(late.destroys == 1);
}
