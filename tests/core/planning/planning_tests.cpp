#include "catalog_builder.hpp"
#include "native_call.hpp"
#include "plan_validation.hpp"
#include "resolution.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

#if defined(_M_X64)
inline constexpr FC_TargetLayout kTestLayout = FC_LAYOUT_CLASSIC_COLLECTION;
inline constexpr FC_Architecture kTestArchitecture = FC_ARCH_X64;
#else
inline constexpr FC_TargetLayout kTestLayout = FC_LAYOUT_STEAM_RETAIL;
inline constexpr FC_Architecture kTestArchitecture = FC_ARCH_X86;
#endif

// Selects the callback behavior needed to exercise normal, malformed, conflicting, and shared hook site plans.
enum class PlanScript {
    Empty,
    Complete,
    EvidenceMatrix,
    BadEvidence,
    WriteConflict,
    ReadConflict,
    OverlappingWrites,
    HookOwner,
    HookObserver,
    DuplicateHook,
    PlanBudgetBoundary,
    PlanBudgetExceeded,
    PlanBudgetSmall,
    NonNullEmptyView,
};

// One retained fixture object is both callback context and non-null patch handle across the lifecycle boundary.
struct PatchCallbackState {
    PlanScript script{PlanScript::Empty};
    std::size_t create_count{};
    std::size_t destroy_count{};
    std::size_t setting_count{};
    std::uint32_t hook_rva{64};
    std::uint32_t observer_state_size{};
    std::uint32_t observer_state_alignment{};
};

// Production admission callbacks submit one borrowed definition before the common path takes immutable ownership.
const FC_PluginDefinition* g_production_definition{};

FC_CallStatus FC_CALL register_production_fixture(const FC_HostApi*, const FC_RegistrySink* registry,
                                                  const FC_ErrorSink*) {
    return registry->submit(registry->context, g_production_definition) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK
                                                                                              : FC_CALL_FAILED;
}

void release_production_fixture() noexcept {}

// Scoped temporary configuration roots keep production generation behavior isolated between test runs.
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint32_t sequence;
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-planning-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path / "plugins");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

// These view helpers preserve borrowed ABI packets while keeping the plan fixtures focused on operation semantics.
[[nodiscard]] constexpr FC_StringView text(std::string_view value) noexcept {
    return {value.data(), static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] FC_ByteView bytes(std::span<const std::byte> value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] constexpr FC_LocationView location(FC_LocationKind kind, std::uint32_t rva,
                                                 FC_Evidence evidence = {}) noexcept {
    return {.kind = kind, .rva = rva, .evidence = evidence};
}

// Lifecycle callbacks expose copied settings and plan submission without adding installation behavior to these tests.
FC_CallStatus FC_CALL create_patch(void* context, const FC_CreateContext*, const FC_SettingsView* settings,
                                   const FC_ErrorSink*, FC_PatchHandle* output) {
    auto& state = *static_cast<PatchCallbackState*>(context);
    ++state.create_count;
    state.setting_count = settings->count;
    *output = &state;
    return FC_CALL_OK;
}

void FC_CALL destroy_patch(void* context, FC_PatchHandle patch) {
    auto& state = *static_cast<PatchCallbackState*>(context);
    if (patch == &state) {
        ++state.destroy_count;
    }
}

FC_CallStatus FC_CALL build_hook(const FC_HookBuildInput*, const FC_ErrorSink*) {
    return FC_CALL_OK;
}

void FC_CALL hook_callback() {}

void FC_CALL bind_original(void*, std::uintptr_t) {}

void FC_CALL connect_interface(void*, const void*) {}

// A normalized void signature supplies the minimum valid transport for hook and observer plan rows.
[[nodiscard]] FC_NativeCall void_native_call() noexcept {
    static constexpr FC_NativeStorage empty_storage{FC_NATIVE_STORAGE_NONE, FC_REGISTER_NONE, 0};
    return {.struct_size = sizeof(FC_NativeCall),
            .result = {FC_NATIVE_VOID, 0, 0},
            .return_storage = empty_storage,
            .cleanup = FC_STACK_CLEANUP_NONE};
}

// Exercises each operation accepted by the plan sink without repeating SDK overload mechanics covered elsewhere.
FC_CallStatus submit_complete_plan(const FC_PlanSink& sink) {
    // Requirements cover each semantic location and both read-only and writable claim policies.
    std::uintptr_t resolved{};
    FC_RequireRequest data_requirement{.struct_size = sizeof(FC_RequireRequest),
                                       .location = location(FC_LOCATION_DATA, 128),
                                       .size = 4,
                                       .alignment = 4,
                                       .writable = FC_TRUE};
    if (sink.require(sink.context, &data_requirement, &resolved) != FC_SUBMIT_ACCEPTED || resolved == 0) {
        return FC_CALL_FAILED;
    }
    FC_RequireRequest code_requirement{.struct_size = sizeof(FC_RequireRequest),
                                       .location = location(FC_LOCATION_CODE, 96),
                                       .size = 1,
                                       .alignment = 1,
                                       .writable = FC_FALSE};
    if (sink.require(sink.context, &code_requirement, &resolved) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }
    FC_RequireRequest function_requirement{.struct_size = sizeof(FC_RequireRequest),
                                           .location = location(FC_LOCATION_FUNCTION, 100),
                                           .size = 1,
                                           .alignment = 1,
                                           .writable = FC_FALSE};
    if (sink.require(sink.context, &function_requirement, &resolved) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }

    // Literal and symbolic writes cover fixed image addresses, plugin code, and each rel32 branch encoding.
    const std::array literal{std::byte{0x44}};
    FC_WriteRequest literal_write{.struct_size = sizeof(FC_WriteRequest),
                                  .location = location(FC_LOCATION_DATA, 132),
                                  .kind = FC_WRITE_BYTES,
                                  .bytes = bytes(literal)};
    if (sink.write(sink.context, &literal_write) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }
    const FC_AddressTarget image_target{.kind = FC_ADDRESS_IMAGE, .image_rva = 32};
    const auto submit_address_write = [&](std::uint32_t rva, FC_LocationKind location_kind, FC_WriteKind kind) {
        const FC_WriteRequest request{.struct_size = sizeof(FC_WriteRequest),
                                      .location = location(location_kind, rva),
                                      .kind = kind,
                                      .target = image_target};
        return sink.write(sink.context, &request) == FC_SUBMIT_ACCEPTED;
    };
    if (!submit_address_write(136, FC_LOCATION_DATA, FC_WRITE_POINTER) ||
        !submit_address_write(148, FC_LOCATION_DATA, FC_WRITE_REL32) ||
        !submit_address_write(40, FC_LOCATION_CODE, FC_WRITE_CALL) ||
        !submit_address_write(48, FC_LOCATION_CODE, FC_WRITE_JUMP)) {
        return FC_CALL_FAILED;
    }
    const FC_AddressTarget plugin_target{
        .kind = FC_ADDRESS_PLUGIN_FUNCTION,
        .plugin_function = reinterpret_cast<std::uintptr_t>(&hook_callback),
    };
    const FC_WriteRequest relayed_call{.struct_size = sizeof(FC_WriteRequest),
                                       .location = location(FC_LOCATION_CODE, 112),
                                       .kind = FC_WRITE_CALL,
                                       .target = plugin_target};
    if (sink.write(sink.context, &relayed_call) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }

    // Control-flow operations include in-place NOP replacement and preservation of an existing branch target.
    const FC_NopRequest nop{
        .struct_size = sizeof(FC_NopRequest), .location = location(FC_LOCATION_CODE, 56), .size = 2};
    if (sink.nop(sink.context, &nop) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }
    const FC_RedirectRequest redirect{.struct_size = sizeof(FC_RedirectRequest),
                                      .location = location(FC_LOCATION_CODE, 104),
                                      .kind = FC_REDIRECT_CALL,
                                      .target = plugin_target};
    std::uintptr_t original{};
    if (sink.redirect(sink.context, &redirect, &original) != FC_SUBMIT_ACCEPTED || original == 0) {
        return FC_CALL_FAILED;
    }

    // A symbolic handle proves later operations can reference framework-owned storage before it has a native address.
    const std::array initial{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const FC_DataAllocationRequest allocation{.struct_size = sizeof(FC_DataAllocationRequest),
                                              .byte_size = initial.size(),
                                              .alignment = 4,
                                              .initial_bytes = bytes(initial),
                                              .name = text("fixture allocation")};
    FC_DataHandle handle{};
    if (sink.allocate_data(sink.context, &allocation, &handle) != FC_SUBMIT_ACCEPTED ||
        handle == FC_INVALID_DATA_HANDLE) {
        return FC_CALL_FAILED;
    }
    const FC_WriteRequest symbolic_write{.struct_size = sizeof(FC_WriteRequest),
                                         .location = location(FC_LOCATION_DATA, 160),
                                         .kind = FC_WRITE_POINTER,
                                         .target = {.kind = FC_ADDRESS_DATA, .data = handle, .data_offset = 2}};
    if (sink.write(sink.context, &symbolic_write) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }

    // Interface and hook records complete the non-mutating plan surface and retained callback metadata.
    const FC_InterfaceBindingRequest binding{.struct_size = sizeof(FC_InterfaceBindingRequest),
                                             .provider_patch = text("OptionalProvider"),
                                             .id = text("FixtureInterface"),
                                             .size = 8,
                                             .connect = &connect_interface};
    if (sink.bind_interface(sink.context, &binding) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }
    const auto call = void_native_call();
    const FC_HookRequest hook{.struct_size = sizeof(FC_HookRequest),
                              .location = location(FC_LOCATION_FUNCTION, 64),
                              .kind = FC_HOOK_FUNCTION_ENTRY,
                              .native_call = &call,
                              .builder = {.build = &build_hook, .entry_size = 1},
                              .callback = reinterpret_cast<std::uintptr_t>(&hook_callback),
                              .bind_original = &bind_original};
    if (sink.hook(sink.context, &hook) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }
    const FC_ObserverRequest observer{.struct_size = sizeof(FC_ObserverRequest),
                                      .location = location(FC_LOCATION_CODE, 80),
                                      .kind = FC_HOOK_INSTRUCTION,
                                      .builder = {.build = &build_hook, .entry_size = 1},
                                      .before = reinterpret_cast<std::uintptr_t>(&hook_callback)};
    return sink.observe(sink.context, &observer) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
}

// The native Plan callback selects complete, malformed, or conflicting submissions through one production sink.
FC_CallStatus FC_CALL plan_patch(void*, FC_PatchHandle patch, const FC_PlanContext*, const FC_PlanSink* sink,
                                 const FC_ErrorSink*) {
    // Scripts send malformed and conflicting native packets through the same sink path as a complete patch plan.
    const auto& state = *static_cast<PatchCallbackState*>(patch);
    const auto script = state.script;
    if (script == PlanScript::Empty) {
        return FC_CALL_OK;
    }
    if (script == PlanScript::Complete) {
        return submit_complete_plan(*sink);
    }
    if (script == PlanScript::EvidenceMatrix) {
        // Each row pairs an evidence form with the extent required to create the corresponding read claim.
        const std::array exact_bytes{std::byte{0x90}};
        const std::array masked_bytes{std::byte{0x90}};
        const std::array mask{std::byte{0xf0}};
        const std::array evidence_rows{
            std::pair{location(FC_LOCATION_CODE, 0, {.kind = FC_EVIDENCE_EXACT_BYTES, .bytes = bytes(exact_bytes)}),
                      std::uint64_t{1}},
            std::pair{location(FC_LOCATION_CODE, 1,
                               {.kind = FC_EVIDENCE_MASKED_BYTES, .bytes = bytes(masked_bytes), .mask = bytes(mask)}),
                      std::uint64_t{1}},
            std::pair{location(FC_LOCATION_DATA, 200, {.kind = FC_EVIDENCE_POINTS_TO, .target_rva = 32}),
                      static_cast<std::uint64_t>(sizeof(std::uintptr_t))},
            std::pair{location(FC_LOCATION_CODE, 16, {.kind = FC_EVIDENCE_DIRECT_CALL_TO, .target_rva = 32}),
                      std::uint64_t{5}},
            std::pair{location(FC_LOCATION_CODE, 24, {.kind = FC_EVIDENCE_DIRECT_JUMP_TO, .target_rva = 32}),
                      std::uint64_t{5}},
        };
        for (const auto& [evidence_location, size] : evidence_rows) {
            const FC_RequireRequest request{.struct_size = sizeof(FC_RequireRequest),
                                            .location = evidence_location,
                                            .size = size,
                                            .alignment = 1,
                                            .writable = FC_FALSE};
            std::uintptr_t output{};
            if (sink->require(sink->context, &request, &output) != FC_SUBMIT_ACCEPTED) {
                return FC_CALL_FAILED;
            }
        }
        return FC_CALL_OK;
    }
    if (script == PlanScript::BadEvidence) {
        // The wrong byte must be rejected by the same evidence path through the mapped image as valid rows.
        const std::array expected{std::byte{0x7f}};
        const FC_Evidence evidence{.kind = FC_EVIDENCE_EXACT_BYTES, .bytes = bytes(expected)};
        const FC_RequireRequest request{.struct_size = sizeof(FC_RequireRequest),
                                        .location = location(FC_LOCATION_CODE, 8, evidence),
                                        .size = 1,
                                        .alignment = 1,
                                        .writable = FC_FALSE};
        std::uintptr_t output{};
        return sink->require(sink->context, &request, &output) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
    }
    if (script == PlanScript::HookOwner || script == PlanScript::HookObserver || script == PlanScript::DuplicateHook) {
        // All hook scripts target one physical site so aggregation, owner conflicts, and duplicate submission interact.
        const auto call = void_native_call();
        if (script == PlanScript::HookOwner) {
            const FC_HookRequest request{.struct_size = sizeof(FC_HookRequest),
                                         .location = location(FC_LOCATION_FUNCTION, 64),
                                         .kind = FC_HOOK_FUNCTION_ENTRY,
                                         .native_call = &call,
                                         .builder = {.build = &build_hook, .entry_size = 1},
                                         .callback = reinterpret_cast<std::uintptr_t>(&hook_callback),
                                         .bind_original = &bind_original};
            return sink->hook(sink->context, &request) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
        }
        const FC_ObserverRequest request{
            .struct_size = sizeof(FC_ObserverRequest),
            .location = location(FC_LOCATION_FUNCTION, state.hook_rva),
            .kind = FC_HOOK_FUNCTION_ENTRY,
            .native_call = &call,
            .builder = {.build = &build_hook, .entry_size = 1},
            .before = reinterpret_cast<std::uintptr_t>(&hook_callback),
            .after = state.observer_state_size == 0 ? 0 : reinterpret_cast<std::uintptr_t>(&hook_callback),
            .state_size = state.observer_state_size,
            .state_alignment = state.observer_state_alignment};
        if (sink->observe(sink->context, &request) != FC_SUBMIT_ACCEPTED) {
            return FC_CALL_FAILED;
        }
        if (script == PlanScript::DuplicateHook) {
            (void)sink->observe(sink->context, &request);
        }
        return FC_CALL_OK;
    }
    if (script == PlanScript::PlanBudgetBoundary || script == PlanScript::PlanBudgetExceeded) {
        // One allocation isolates copied bytes plus its fixed operation record at and just beyond the plan budget.
        const auto payload_size = fc::planning::kPatchPlanByteCapacity - sizeof(fc::planning::OperationRecord) +
                                  (script == PlanScript::PlanBudgetExceeded ? 1U : 0U);
        const std::vector<std::byte> initial(payload_size);
        const FC_DataAllocationRequest request{.struct_size = sizeof(FC_DataAllocationRequest),
                                               .byte_size = initial.size(),
                                               .alignment = 1,
                                               .initial_bytes = bytes(initial)};
        FC_DataHandle output{};
        return sink->allocate_data(sink->context, &request, &output) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK
                                                                                           : FC_CALL_FAILED;
    }
    if (script == PlanScript::PlanBudgetSmall) {
        // A minimum allocation supplies the first copied bytes beyond a validation run's aggregate budget.
        static constexpr std::byte initial{};
        const FC_DataAllocationRequest request{.struct_size = sizeof(FC_DataAllocationRequest),
                                               .byte_size = 1,
                                               .alignment = 1,
                                               .initial_bytes = {reinterpret_cast<const std::uint8_t*>(&initial), 1}};
        FC_DataHandle output{};
        return sink->allocate_data(sink->context, &request, &output) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK
                                                                                           : FC_CALL_FAILED;
    }
    if (script == PlanScript::NonNullEmptyView) {
        // The ABI ignores the pointer of a zero-length leaf view, including an inactive allocation payload.
        static constexpr std::byte ignored{};
        const FC_DataAllocationRequest request{.struct_size = sizeof(FC_DataAllocationRequest),
                                               .byte_size = 1,
                                               .alignment = 1,
                                               .initial_bytes = {reinterpret_cast<const std::uint8_t*>(&ignored), 0}};
        FC_DataHandle output{};
        return sink->allocate_data(sink->context, &request, &output) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK
                                                                                           : FC_CALL_FAILED;
    }

    // Conflict scripts share RVAs across patches; OverlappingWrites creates a defect within one patch plan.
    const auto rva = script == PlanScript::WriteConflict || script == PlanScript::ReadConflict ? 176U : 184U;
    if (script == PlanScript::ReadConflict) {
        const FC_RequireRequest request{.struct_size = sizeof(FC_RequireRequest),
                                        .location = location(FC_LOCATION_DATA, rva),
                                        .size = 4,
                                        .alignment = 1,
                                        .writable = FC_FALSE};
        std::uintptr_t output{};
        return sink->require(sink->context, &request, &output) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
    }
    const std::array replacement{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const FC_WriteRequest first{.struct_size = sizeof(FC_WriteRequest),
                                .location = location(FC_LOCATION_DATA, rva),
                                .kind = FC_WRITE_BYTES,
                                .bytes = bytes(replacement)};
    if (sink->write(sink->context, &first) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }
    if (script == PlanScript::OverlappingWrites) {
        const FC_WriteRequest second{.struct_size = sizeof(FC_WriteRequest),
                                     .location = location(FC_LOCATION_DATA, rva + 2),
                                     .kind = FC_WRITE_BYTES,
                                     .bytes = bytes(replacement)};
        return sink->write(sink->context, &second) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
    }
    return FC_CALL_OK;
}

// These builders retain the minimum selected metadata needed to vary resolution and validation behavior by script.
[[nodiscard]] fc::catalog::PatchDefinitionRecord patch(std::string id, PatchCallbackState& state, bool enabled = false,
                                                       bool configurable = false,
                                                       FC_TargetImage image = FC_IMAGE_GAME) {
    fc::catalog::SupportDefinitionRecord support{
        .layouts = kTestLayout,
        .roles = FC_HOST_ROLE_CLIENT,
        .image = image,
        .callbacks = {.context = &state, .create = &create_patch, .destroy = &destroy_patch, .plan = &plan_patch}};
    fc::catalog::PatchDefinitionRecord result{.id = std::move(id),
                                              .name = "Fixture patch",
                                              .configurable = configurable ? FC_TRUE : FC_FALSE,
                                              .enabled = enabled ? FC_TRUE : FC_FALSE,
                                              .failure_policy = FC_FAILURE_CONTINUE,
                                              .supports = {std::move(support)},
                                              .selected_support = 0};
    return result;
}

[[nodiscard]] fc::catalog::PluginRecord plugin(std::string id, std::vector<fc::catalog::PatchDefinitionRecord> patches,
                                               std::vector<fc::catalog::GroupDefinitionRecord> groups = {}) {
    auto owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&plan_patch));
    REQUIRE(owner.has_value());
    fc::catalog::PluginRecord result;
    result.sdk_revision = FC_SDK_REVISION;
    result.code_owner = std::move(*owner);
    result.definition.id = std::move(id);
    result.definition.groups = std::move(groups);
    result.definition.patches = std::move(patches);
    return result;
}

[[nodiscard]] fc::config::ConfigurationSnapshot configuration(const fc::catalog::Catalog& catalog) {
    // Reproduce the alignment guarantee normally established by final admission without introducing file I/O here.
    fc::config::ConfigurationSnapshot result;
    for (const auto& plugin_record : catalog.plugins()) {
        fc::config::PluginConfiguration plugin_configuration{.plugin_id = plugin_record.definition.id};
        plugin_configuration.patches.resize(plugin_record.definition.patches.size());
        plugin_configuration.groups.resize(plugin_record.definition.groups.size());
        for (std::size_t index = 0; index < plugin_record.definition.patches.size(); ++index) {
            const auto& definition = plugin_record.definition.patches[index];
            const auto& support = definition.supports[*definition.selected_support];
            const auto setting_count =
                support.has_settings == FC_TRUE ? support.settings.size() : definition.settings.size();
            plugin_configuration.patches[index].settings.resize(setting_count);
        }
        result.plugins.push_back(std::move(plugin_configuration));
    }
    return result;
}

[[nodiscard]] fc::targets::RecognizedTarget target() {
    // The synthetic mapping supplies reviewed role/layout identity while keeping every exercised range small and owned.
    auto game_bytes = std::vector<std::byte>(256, std::byte{0x90});
    const std::int32_t call_displacement = 32 - (16 + 5);
    game_bytes[16] = std::byte{0xe8};
    std::memcpy(game_bytes.data() + 17, &call_displacement, sizeof(call_displacement));
    const std::int32_t jump_displacement = 32 - (24 + 5);
    game_bytes[24] = std::byte{0xe9};
    std::memcpy(game_bytes.data() + 25, &jump_displacement, sizeof(jump_displacement));
    // Redirects preserve valid external original callees even though verifier mappings contain only the selected image.
    const std::int32_t external_call_displacement = 300 - (104 + 5);
    game_bytes[104] = std::byte{0xe8};
    std::memcpy(game_bytes.data() + 105, &external_call_displacement, sizeof(external_call_displacement));
    const auto pointed_address = reinterpret_cast<std::uintptr_t>(game_bytes.data()) + 32;
    std::memcpy(game_bytes.data() + 200, &pointed_address, sizeof(pointed_address));
    std::vector<fc::targets::OwnedImage> images;
#if defined(_M_X64)
    auto bootstrap_bytes = std::vector<std::byte>(256, std::byte{0x90});
    images.push_back(fc::targets::OwnedImage::mapped(
        {FC_IMAGE_BOOTSTRAP, "ClassicCollection_Bootstrap_66702CD7", 0, bootstrap_bytes.size()},
        std::move(bootstrap_bytes), {{128, 128}}, {{0, 256}}, {{0, 128}}));
    constexpr std::string_view game_profile = "ClassicCollection_Game_66702CD2";
#else
    constexpr std::string_view game_profile = "SteamRetail_Game_59EDE353";
#endif
    images.push_back(fc::targets::OwnedImage::mapped({FC_IMAGE_GAME, game_profile, 0, game_bytes.size()},
                                                     std::move(game_bytes), {{128, 128}}, {{0, 256}}, {{0, 128}}));
    auto recognized = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(images));
    REQUIRE(recognized.has_value());
    return std::move(*recognized);
}

[[nodiscard]] fc::planning::PatchState state(const fc::planning::PatchWorkSet& work, std::string_view id) {
    const auto patch_index = work.catalog().find_patch(id);
    REQUIRE(patch_index.has_value());
    return work.record(*patch_index).state;
}

// Captures scoped framework diagnostics without asynchronous file output while preserving level and component context.
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

} // namespace

TEST_CASE("resolution applies fixed precedence, relationships, settings, cycles, and waiting states once") {
    // This graph combines every terminal selection cause so propagation can be checked without separate closures.
    std::array<PatchCallbackState, 13> callbacks{};
    auto fixed_off = patch("FixedOff", callbacks[0], true, true);
    auto group_member = patch("GroupMember", callbacks[1]);
    group_member.includes = {"Included"};
    auto fixed_member = patch("FixedMember", callbacks[2], true, true);
    auto included = patch("Included", callbacks[3]);
    included.depends_on = {"CrossPluginProvider"};
    // This back edge proves an already skipped patch cannot create a false dependency cycle for its consumer.
    auto missing = patch("Missing", callbacks[4], true);
    missing.depends_on = {"AbsentRelationship", "Consumer"};
    auto consumer = patch("Consumer", callbacks[5], true);
    consumer.depends_on = {"Missing"};
    auto cycle_a = patch("CycleA", callbacks[6], true);
    cycle_a.depends_on = {"CycleB"};
    auto cycle_b = patch("CycleB", callbacks[7], true);
    cycle_b.depends_on = {"CycleA"};
    auto bad_setting = patch("BadSetting", callbacks[8], true);
    bad_setting.settings.push_back({.key = "Count", .type = FC_SETTING_UNSIGNED_8});
    auto waiting = patch("Waiting", callbacks[9], true, false, FC_IMAGE_GALAXY_PEER);
    auto waiting_consumer = patch("WaitingConsumer", callbacks[10], true);
    waiting_consumer.depends_on = {"Waiting"};
    auto group_consumer = patch("GroupConsumer", callbacks[11], true);
    group_consumer.depends_on = {"SelectionGroup"};

    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(
        plugin("SelectionFixture",
               {std::move(fixed_off), std::move(group_member), std::move(fixed_member), std::move(included),
                std::move(missing), std::move(consumer), std::move(cycle_a), std::move(cycle_b), std::move(bad_setting),
                std::move(waiting), std::move(waiting_consumer), std::move(group_consumer)},
               {{.id = "SelectionGroup",
                 .members = {"GroupMember", "FixedMember", "FixedOff"},
                 .configurable = FC_TRUE,
                 .enabled = FC_TRUE}}));
    plugins.push_back(plugin("ProviderFixture", {patch("CrossPluginProvider", callbacks[12])}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    configured.plugins[0].patches[0].toggle = fc::config::StoredValue{"false", 1};
    configured.plugins[0].patches[2].toggle = fc::config::StoredValue{"false", 2};
    configured.plugins[0].patches[8].settings[0].ini = fc::config::StoredValue{"999", 3};
    auto recognized = target();

    LogCapture logs;
    auto work = fc::planning::resolve_patches(
        {.catalog = catalog, .target = recognized, .configuration = configured, .logger = logs.logger("Selection")});
    // Direct decisions, transitive selection, and independent failures must all remain frozen in the final work set.
    CHECK(state(work, "FixedOff") == fc::planning::PatchState::Disabled);
    CHECK(state(work, "GroupMember") == fc::planning::PatchState::Pending);
    CHECK(state(work, "FixedMember") == fc::planning::PatchState::Disabled);
    CHECK(state(work, "Included") == fc::planning::PatchState::Pending);
    CHECK(state(work, "CrossPluginProvider") == fc::planning::PatchState::Pending);
    CHECK(state(work, "Missing") == fc::planning::PatchState::Skipped);
    CHECK(state(work, "Consumer") == fc::planning::PatchState::Skipped);
    CHECK(state(work, "CycleA") == fc::planning::PatchState::Failed);
    CHECK(state(work, "CycleB") == fc::planning::PatchState::Failed);
    CHECK(state(work, "BadSetting") == fc::planning::PatchState::Failed);
    CHECK(state(work, "Waiting") == fc::planning::PatchState::WaitingForImage);
    CHECK(state(work, "WaitingConsumer") == fc::planning::PatchState::Skipped);
    const auto& waiting_reason = work.record(*catalog.find_patch("WaitingConsumer")).reason;
    REQUIRE(waiting_reason.has_value());
    CHECK(waiting_reason->phase == fc::planning::PatchPhase::Selection);
    CHECK(waiting_reason->related_patch == std::optional<std::string_view>{"Waiting"});
    CHECK(state(work, "GroupConsumer") == fc::planning::PatchState::Skipped);
    const auto& group_reason = work.record(*catalog.find_patch("GroupConsumer")).reason;
    REQUIRE(group_reason.has_value());
    CHECK(group_reason->related_patch == std::optional<std::string_view>{"FixedMember"});
    CHECK(group_reason->related_group == std::optional<std::string_view>{"SelectionGroup"});
    const auto& missing_reason = work.record(*catalog.find_patch("Missing")).reason;
    REQUIRE(missing_reason.has_value());
    CHECK(missing_reason->phase == fc::planning::PatchPhase::Selection);
    CHECK_FALSE(missing_reason->related_patch.has_value());
    CHECK_FALSE(missing_reason->related_group.has_value());
    // Settled outcomes provide useful severity and identity without narrating the intermediate closure passes.
    CHECK_FALSE(logs.allocation_failed);
    CHECK(logs.contains("Selection", FC_LOG_DEBUG, "Selected patch 'GroupMember'"));
    CHECK(logs.contains("Selection", FC_LOG_WARNING, "Patch 'Missing' was skipped during selection"));
    CHECK(logs.contains("Selection", FC_LOG_WARNING, "Patch 'Consumer' was skipped during selection"));
    CHECK(logs.contains("Selection", FC_LOG_INFO, "Selected"));
}

TEST_CASE("production admission transfers directly into the common non-mutating validation owner") {
    // Borrowed native definitions cross CatalogBuilder before planning, matching a production bundled plugin.
    PatchCallbackState callback;
    const FC_SupportDefinition support{
        .layouts = kTestLayout,
        .roles = FC_HOST_ROLE_CLIENT,
        .image = FC_IMAGE_GAME,
        .callbacks = {.context = &callback, .create = &create_patch, .destroy = &destroy_patch, .plan = &plan_patch},
        .failure_policy = FC_FAILURE_INHERIT};
    const FC_PatchDefinition native_patch{.id = text("ProductionPlan"),
                                          .name = text("Production planning fixture"),
                                          .enabled = FC_TRUE,
                                          .failure_policy = FC_FAILURE_CONTINUE,
                                          .supports = &support,
                                          .support_count = 1};
    const FC_PluginDefinition native_plugin{.struct_size = sizeof(FC_PluginDefinition),
                                            .id = text("ProductionPlanningFixture"),
                                            .patches = &native_patch,
                                            .patch_count = 1};
    g_production_definition = &native_plugin;
    const FC_HostApi host{.struct_size = sizeof(FC_HostApi)};
    auto owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&register_production_fixture));
    REQUIRE(owner.has_value());
    auto recognized = target();
    TemporaryDirectory temporary;

    fc::catalog::CatalogBuilder builder{host, std::move(*owner)};
    builder.add_core(fc::catalog::core_registration_bridge());
    builder.add_bundled({&register_production_fixture, &release_production_fixture});
    auto acquired = builder.build(recognized, fc::config::ConfigurationPaths{temporary.path});
    REQUIRE(acquired.catalog.has_value());
    REQUIRE(acquired.configuration.has_value());

    // Successful admission feeds the same resolution and plan collector used by synthetic fixtures.
    LogCapture logs;
    auto work = fc::planning::resolve_patches({.catalog = *acquired.catalog,
                                               .target = recognized,
                                               .configuration = *acquired.configuration,
                                               .logger = logs.logger("Selection")});
    const auto installation = fc::planning::build_installation_plan(recognized, work, {}, logs.logger("Validation"));
    CHECK(state(work, "ProductionPlan") == fc::planning::PatchState::Ready);
    CHECK(installation.installation_order.size() == 1);
    CHECK_FALSE(logs.allocation_failed);
    CHECK(logs.contains("Validation", FC_LOG_DEBUG, "Patch 'ProductionPlan' is in the Ready state"));
    CHECK(logs.contains("Validation", FC_LOG_INFO, "Validated 1 patch(es) for installation"));
}

TEST_CASE("synthetic Testing input and verifier input from a mapped image share complete non-mutating planning") {
    PatchCallbackState callback{.script = PlanScript::Complete};
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(plugin("CompleteFixture", {patch("CompletePlan", callback, true)}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();
    std::array<std::byte, 256> before{};
    REQUIRE(recognized.find(FC_IMAGE_GAME)->read({0}, before).has_value());

    // The complete patch plan is retained and aggregated, but planning cannot publish requested mutations.
    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    const auto patch_index = *catalog.find_patch("CompletePlan");
    CHECK(work.record(patch_index).state == fc::planning::PatchState::Ready);
    CHECK(work.record(patch_index).plan.operations.size() == 16);
    CHECK(installation.hook_aggregates.size() == 2);
    CHECK(installation.installation_order == std::vector{patch_index});
    CHECK(callback.create_count == 1);
    CHECK(callback.setting_count == 0);

    std::array<std::byte, 256> after{};
    REQUIRE(recognized.find(FC_IMAGE_GAME)->read({0}, after).has_value());
    CHECK(std::memcmp(after.data(), before.data(), before.size()) == 0);
}

TEST_CASE("evidence, dependency pruning, and frozen conflicts isolate unrelated plans") {
    // The scripts combine a root Plan callback failure, its consumers, conflicting claims, and an unrelated survivor.
    std::array<PatchCallbackState, 8> callbacks{{
        {.script = PlanScript::BadEvidence},
        {.script = PlanScript::Empty},
        {.script = PlanScript::Empty},
        {.script = PlanScript::Empty},
        {.script = PlanScript::WriteConflict},
        {.script = PlanScript::ReadConflict},
        {.script = PlanScript::OverlappingWrites},
        {.script = PlanScript::Empty},
    }};
    auto evidence_consumer = patch("EvidenceConsumer", callbacks[1], true);
    evidence_consumer.depends_on = {"EvidenceFailure"};
    auto transitive_consumer = patch("TransitiveConsumer", callbacks[2], true);
    transitive_consumer.depends_on = {"EvidenceConsumer"};
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(
        plugin("FailureFixture", {patch("EvidenceFailure", callbacks[0], true), std::move(evidence_consumer),
                                  std::move(transitive_consumer), patch("Independent", callbacks[3], true),
                                  patch("ConflictWrite", callbacks[4], true), patch("ConflictRead", callbacks[5], true),
                                  patch("OwnOverlap", callbacks[6], true), patch("Unused", callbacks[7])}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();

    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    // Failures propagate through required edges, while frozen claim conflicts remove both participants without rescue.
    CHECK(state(work, "EvidenceFailure") == fc::planning::PatchState::Failed);
    CHECK(state(work, "EvidenceConsumer") == fc::planning::PatchState::Skipped);
    CHECK(state(work, "TransitiveConsumer") == fc::planning::PatchState::Skipped);
    const auto& transitive_reason = work.record(*catalog.find_patch("TransitiveConsumer")).reason;
    REQUIRE(transitive_reason.has_value());
    CHECK(transitive_reason->phase == fc::planning::PatchPhase::Plan);
    CHECK(transitive_reason->related_patch == std::optional<std::string_view>{"EvidenceConsumer"});
    CHECK(state(work, "Independent") == fc::planning::PatchState::Ready);
    CHECK(state(work, "ConflictWrite") == fc::planning::PatchState::Failed);
    CHECK(state(work, "ConflictRead") == fc::planning::PatchState::Failed);
    CHECK(state(work, "OwnOverlap") == fc::planning::PatchState::Failed);
    CHECK(state(work, "Unused") == fc::planning::PatchState::Disabled);
    REQUIRE(installation.installation_order.size() == 1);
    CHECK(catalog.patch(installation.installation_order.front()).id == "Independent");
    CHECK(callbacks[0].destroy_count == 1);
    CHECK(callbacks[1].create_count == 1);
    CHECK(callbacks[1].destroy_count == 1);
    CHECK(callbacks[2].create_count == 1);
    CHECK(callbacks[2].destroy_count == 1);
}

TEST_CASE("copied plan capacity accepts the exact boundary and rejects one additional byte") {
    std::array<PatchCallbackState, 2> callbacks{{
        {.script = PlanScript::PlanBudgetBoundary},
        {.script = PlanScript::PlanBudgetExceeded},
    }};
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(plugin("PlanBudgetFixture",
                             {patch("AtBoundary", callbacks[0], true), patch("BeyondBoundary", callbacks[1], true)}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();

    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    CHECK(state(work, "AtBoundary") == fc::planning::PatchState::Ready);
    CHECK(state(work, "BeyondBoundary") == fc::planning::PatchState::Failed);
    REQUIRE(installation.installation_order.size() == 1);
    CHECK(catalog.patch(installation.installation_order.front()).id == "AtBoundary");
}

TEST_CASE("one validation run accepts 64 MiB of copied plans and rejects its next operation") {
    constexpr auto boundary_patch_count =
        fc::planning::kValidationPlanByteCapacity / fc::planning::kPatchPlanByteCapacity;
    std::array<PatchCallbackState, boundary_patch_count + 1> callbacks;
    std::vector<fc::catalog::PatchDefinitionRecord> patches;
    patches.reserve(callbacks.size());
    for (std::size_t index = 0; index < boundary_patch_count; ++index) {
        callbacks[index].script = PlanScript::PlanBudgetBoundary;
        const auto suffix = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
        patches.push_back(patch("Budget" + suffix, callbacks[index], true));
    }
    callbacks.back().script = PlanScript::PlanBudgetSmall;
    patches.push_back(patch("BudgetBeyond", callbacks.back(), true));
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(plugin("ValidationBudgetFixture", std::move(patches)));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();

    // Stable patch-ID order fills the aggregate exactly before the final patch contributes one more operation.
    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    CHECK(installation.installation_order.size() == boundary_patch_count);
    CHECK(state(work, "Budget31") == fc::planning::PatchState::Ready);
    CHECK(state(work, "BudgetBeyond") == fc::planning::PatchState::Failed);
}

TEST_CASE("zero-length Plan leaf views ignore a non-null pointer") {
    PatchCallbackState callback{.script = PlanScript::NonNullEmptyView};
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(plugin("EmptyViewFixture", {patch("EmptyView", callback, true)}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();

    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    CHECK(state(work, "EmptyView") == fc::planning::PatchState::Ready);
    CHECK(installation.installation_order.size() == 1);
}

TEST_CASE("every evidence form validates through the same mapped image path") {
    PatchCallbackState callback{.script = PlanScript::EvidenceMatrix};
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(plugin("EvidenceFixture", {patch("EvidenceMatrix", callback, true)}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();

    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    CHECK(state(work, "EvidenceMatrix") == fc::planning::PatchState::Ready);
    CHECK(installation.installation_order.size() == 1);
}

TEST_CASE("installed baseline claims remain immutable and reject only the new claimant") {
    std::array<PatchCallbackState, 2> callbacks{{{}, {.script = PlanScript::WriteConflict}}};
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(
        plugin("BaselineFixture", {patch("InstalledPatch", callbacks[0]), patch("LateClaimant", callbacks[1], true)}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();
    const auto installed_patch = *catalog.find_patch("InstalledPatch");
    const std::array installed_claims{
        fc::planning::MemoryClaim{installed_patch, FC_IMAGE_GAME, 176, 4, fc::planning::ClaimAccess::Read, 0}};

    // Preinstalled state is an immutable blocker, not a candidate that validation may terminalize or reorder.
    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    work.record(installed_patch).state = fc::planning::PatchState::Installed;
    const auto installation =
        fc::planning::build_installation_plan(recognized, work, {.installed_claims = installed_claims});
    CHECK(state(work, "LateClaimant") == fc::planning::PatchState::Failed);
    CHECK(installation.installation_order.empty());
}

TEST_CASE("conflicting hook owners fail while compatible observers retain the physical site") {
    // Two owners, one observer, and one duplicate contribution all target the same physical function entry.
    std::array<PatchCallbackState, 4> callbacks{{
        {.script = PlanScript::HookOwner},
        {.script = PlanScript::HookOwner},
        {.script = PlanScript::HookObserver},
        {.script = PlanScript::DuplicateHook},
    }};
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(
        plugin("HookFixture",
               {patch("FirstOwner", callbacks[0], true), patch("SecondOwner", callbacks[1], true),
                patch("StableObserver", callbacks[2], true), patch("DuplicateContribution", callbacks[3], true)}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();

    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    // Owner defects do not erase a compatible observer-only aggregate at the reviewed site.
    CHECK(state(work, "FirstOwner") == fc::planning::PatchState::Failed);
    CHECK(state(work, "SecondOwner") == fc::planning::PatchState::Failed);
    CHECK(state(work, "StableObserver") == fc::planning::PatchState::Ready);
    CHECK(state(work, "DuplicateContribution") == fc::planning::PatchState::Failed);
    REQUIRE(installation.hook_aggregates.size() == 1);
    CHECK_FALSE(installation.hook_aggregates.front().owner.has_value());
    CHECK(installation.hook_aggregates.front().observers.size() == 1);
}

TEST_CASE("hook observer capacity retains the stable ID prefix without backtracking") {
    // Seventeen alphabetically sortable observers exceed the sixteen-entry site capacity by exactly one.
    std::array<PatchCallbackState, 17> callbacks;
    std::vector<fc::catalog::PatchDefinitionRecord> patches;
    patches.reserve(callbacks.size());
    for (std::size_t index = 0; index < callbacks.size(); ++index) {
        callbacks[index].script = PlanScript::HookObserver;
        const auto suffix = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
        patches.push_back(patch("Observer" + suffix, callbacks[index], true));
    }
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(plugin("HookCapacityFixture", std::move(patches)));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();

    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    CHECK(state(work, "Observer15") == fc::planning::PatchState::Ready);
    CHECK(state(work, "Observer16") == fc::planning::PatchState::Failed);
    REQUIRE(installation.hook_aggregates.size() == 1);
    CHECK(installation.hook_aggregates.front().observers.size() == fc::planning::kHookObserverCapacity);
}

TEST_CASE("hook observer state accepts exact size and alignment bounds before rejecting the next values") {
    std::array<PatchCallbackState, 4> callbacks{{
        {.script = PlanScript::HookObserver,
         .hook_rva = 64,
         .observer_state_size = fc::planning::kHookStateByteCapacity,
         .observer_state_alignment = fc::planning::kHookStateAlignmentCapacity},
        {.script = PlanScript::HookObserver, .hook_rva = 64, .observer_state_size = 1, .observer_state_alignment = 1},
        {.script = PlanScript::HookObserver,
         .hook_rva = 32,
         .observer_state_size = 1,
         .observer_state_alignment = fc::planning::kHookStateAlignmentCapacity},
        {.script = PlanScript::HookObserver,
         .hook_rva = 32,
         .observer_state_size = 1,
         .observer_state_alignment = fc::planning::kHookStateAlignmentCapacity * 2},
    }};
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(
        plugin("HookStateCapacityFixture",
               {patch("AExactState", callbacks[0], true), patch("BStateBeyond", callbacks[1], true),
                patch("CExactAlignment", callbacks[2], true), patch("DAlignmentBeyond", callbacks[3], true)}));
    fc::catalog::Catalog catalog{std::move(plugins)};
    auto configured = configuration(catalog);
    auto recognized = target();

    // The aggregate and per-observer checks are independent: each exact boundary survives its immediate excess row.
    auto work = fc::planning::resolve_patches({catalog, recognized, configured});
    const auto installation = fc::planning::build_installation_plan(recognized, work);
    CHECK(state(work, "AExactState") == fc::planning::PatchState::Ready);
    CHECK(state(work, "BStateBeyond") == fc::planning::PatchState::Failed);
    CHECK(state(work, "CExactAlignment") == fc::planning::PatchState::Ready);
    CHECK(state(work, "DAlignmentBeyond") == fc::planning::PatchState::Failed);
    CHECK(installation.hook_aggregates.size() == 2);
}

TEST_CASE("native call validation enforces architecture, storage, and cleanup as one table") {
    auto valid = void_native_call();
    CHECK(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());

    const auto native_pointer_size = kTestArchitecture == FC_ARCH_X86 ? 4U : 8U;
    const auto result_register = kTestArchitecture == FC_ARCH_X86 ? FC_REGISTER_EAX : FC_REGISTER_RAX;
    const auto argument_register = kTestArchitecture == FC_ARCH_X86 ? FC_REGISTER_ECX : FC_REGISTER_RCX;
    const auto alternate_register = kTestArchitecture == FC_ARCH_X86 ? FC_REGISTER_EBX : FC_REGISTER_RBX;
    const auto wrong_architecture_register = kTestArchitecture == FC_ARCH_X86 ? FC_REGISTER_RAX : FC_REGISTER_EAX;

    // Return homes must match the logical result and the selected architecture's register family.
    valid.result = {FC_NATIVE_POINTER, native_pointer_size, native_pointer_size};
    valid.return_storage = {FC_NATIVE_STORAGE_NONE, FC_REGISTER_NONE, 0};
    CHECK_FALSE(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());
    valid.return_storage = {FC_NATIVE_STORAGE_REGISTER, result_register, 0};
    CHECK(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());

    valid.return_storage = {FC_NATIVE_STORAGE_REGISTER, wrong_architecture_register, 0};
    CHECK_FALSE(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());

    valid = void_native_call();
    valid.cleanup = FC_STACK_CLEANUP_CALLER;
    CHECK_FALSE(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());

    // Arguments may mix register and stack homes, but no physical home can overlap another argument.
    std::array<FC_NativeArgument, 2> arguments{{
        {{FC_NATIVE_POINTER, native_pointer_size, native_pointer_size},
         {FC_NATIVE_STORAGE_REGISTER, argument_register, 0}},
        {{FC_NATIVE_INTEGER, 4, 4}, {FC_NATIVE_STORAGE_STACK, FC_REGISTER_NONE, 0}},
    }};
    valid = void_native_call();
    valid.arguments = arguments.data();
    valid.argument_count = static_cast<std::uint32_t>(arguments.size());
    valid.cleanup = kTestArchitecture == FC_ARCH_X86 ? FC_STACK_CLEANUP_CALLER : FC_STACK_CLEANUP_NONE;
    valid.stack_size = 4;
    CHECK(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());

    arguments[1] = arguments[0];
    CHECK_FALSE(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());
    arguments[1] = {{FC_NATIVE_INTEGER, 4, 4}, {FC_NATIVE_STORAGE_STACK, FC_REGISTER_NONE, 0}};

    // A record result is returned through a hidden pointer that may not reuse an argument home.
    valid.result = {FC_NATIVE_RECORD, native_pointer_size * 2, native_pointer_size};
    valid.return_storage = {FC_NATIVE_STORAGE_REGISTER, argument_register, 0};
    CHECK_FALSE(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());
    valid.return_storage = {FC_NATIVE_STORAGE_REGISTER, alternate_register, 0};
    CHECK(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());

    // Floating-point return storage follows the x87 rule on x86 and the SIMD rule on x64.
    valid = void_native_call();
    valid.result = {FC_NATIVE_FLOAT_64, 8, 8};
    valid.return_storage = {FC_NATIVE_STORAGE_REGISTER,
                            kTestArchitecture == FC_ARCH_X86 ? FC_REGISTER_ST0 : FC_REGISTER_XMM0, 0};
    CHECK(fc::planning::validate_native_call(valid, kTestArchitecture).has_value());
}
