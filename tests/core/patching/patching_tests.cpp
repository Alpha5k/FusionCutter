#include "installation.hpp"
#include "native_address.hpp"
#include "patch_transaction.hpp"
#include "plan_validation.hpp"
#include "resolution.hpp"

#include <FusionCutter/SDK.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
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

// Drives lifecycle callbacks through successful publication and each revalidation or containment failure boundary.
enum class LifecycleScript {
    Empty,
    MutationMatrix,
    PrepareFailure,
    IgnoredResolveFailure,
    EvidenceMutation,
    RawMutation,
    RequiredStale,
    CodeAccessStale,
    CodeAccessMutation,
    RetainedFailure,
    HookOwner,
    HookObserver,
    HookBrokenBuilder,
    DirectHookOwner,
    DirectHookObserver,
    InstructionHookOwner,
    InstructionHookObserver,
};

// Callback state deliberately lives outside PatchRuntimeState so teardown ordering can be asserted after it dies.
struct PatchCallbackState {
    LifecycleScript script{LifecycleScript::Empty};
    std::uintptr_t image_base{};
    std::size_t page_size{};
    FC_DataHandle data_handle{};
    std::uintptr_t data_address{};
    std::uintptr_t required_address{};
    std::uintptr_t original_target{};
    std::uintptr_t hook_original{};
    std::size_t create_count{};
    std::size_t prepare_count{};
    std::size_t activate_count{};
    std::size_t update_count{};
    std::size_t destroy_count{};
    std::size_t hook_calls{};
    std::size_t before_calls{};
    std::size_t after_calls{};
    std::size_t state_mismatches{};
    bool original_bound_during_prepare{};
    bool allocation_live_during_destroy{};
    bool hook_evidence{};
    bool rejected_resolution_cleared_outputs{};
};

using HookCall = std::int32_t(FC_CALL*)(std::int32_t left, std::int32_t right) noexcept;

// These view helpers keep the fixture requests readable while preserving their borrowed ABI representation.
[[nodiscard]] constexpr FC_StringView text(std::string_view value) noexcept {
    return {value.data(), static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] FC_ByteView bytes(std::span<const std::byte> value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] unsigned int byte_value(std::byte value) noexcept {
    return std::to_integer<unsigned int>(value);
}

[[nodiscard]] constexpr FC_LocationView location(FC_LocationKind kind, std::uint32_t rva,
                                                 FC_Evidence evidence = {}) noexcept {
    return {.kind = kind, .rva = rva, .evidence = evidence};
}

void set_callback_error(const FC_ErrorSink* error, std::string_view message, std::string_view operation) {
    if (error != nullptr && error->set != nullptr) {
        error->set(error->context, text(message), text(operation));
    }
}

// Lifecycle callbacks expose creation, destruction, and phase ordering through the external state counters above.
FC_CallStatus FC_CALL create_patch(void* context, const FC_CreateContext*, const FC_SettingsView*, const FC_ErrorSink*,
                                   FC_PatchHandle* output) {
    auto& state = *static_cast<PatchCallbackState*>(context);
    ++state.create_count;
    *output = &state;
    return FC_CALL_OK;
}

void FC_CALL destroy_patch(void* context, FC_PatchHandle patch) {
    auto& state = *static_cast<PatchCallbackState*>(context);
    if (patch != &state) {
        return;
    }
    ++state.destroy_count;
    // Destruction records whether native storage outlives the plugin instance as required by exposed failures.
    if (state.data_address != 0) {
        MEMORY_BASIC_INFORMATION memory{};
        state.allocation_live_during_destroy = VirtualQuery(reinterpret_cast<const void*>(state.data_address), &memory,
                                                            sizeof(memory)) == sizeof(memory) &&
                                               memory.State == MEM_COMMIT;
    }
}

// A real function address gives branch encoders a plugin-owned destination without adding behavior to the fixture.
void FC_CALL branch_destination() {}

// These C thunks model copied owner and observer transport after the request from the Plan callback expires.
std::int32_t FC_CALL hook_owner(void* context, std::int32_t left, std::int32_t right) noexcept {
    auto& state = *static_cast<PatchCallbackState*>(context);
    ++state.hook_calls;
    return reinterpret_cast<HookCall>(state.hook_original)(left, right) + 5;
}

void FC_CALL hook_before(void* context, std::int32_t left, std::int32_t right, void* invocation_state) noexcept {
    auto& state = *static_cast<PatchCallbackState*>(context);
    ++state.before_calls;
    auto& invocation = *static_cast<std::int32_t*>(invocation_state);
    // A fresh zero proves the SDK began a value-initialized State lifetime for this invocation.
    if (invocation != 0) {
        ++state.state_mismatches;
    }
    invocation = left + right;
    SetLastError(0x1111);
}

void FC_CALL hook_after(void* context, std::int32_t left, std::int32_t right, std::int32_t result,
                        const void* invocation_state) noexcept {
    auto& state = *static_cast<PatchCallbackState*>(context);
    ++state.after_calls;
    if (*static_cast<const std::int32_t*>(invocation_state) != left + right || result < left + right) {
        ++state.state_mismatches;
    }
    SetLastError(0x2222);
}

void FC_CALL bind_hook_original(void* context, std::uintptr_t original) {
    static_cast<PatchCallbackState*>(context)->hook_original = original;
}

void FC_CALL instruction_owner(void* context, FC_CpuContext* cpu) noexcept {
    auto& state = *static_cast<PatchCallbackState*>(context);
    ++state.hook_calls;
#if defined(_M_X64)
    cpu->rax = 7;
#else
    cpu->eax = 7;
#endif
    SetLastError(0x3333);
}

void FC_CALL instruction_before(void* context, const FC_CpuContext* cpu, void* invocation_state) noexcept {
    auto& state = *static_cast<PatchCallbackState*>(context);
    ++state.before_calls;
    auto& invocation = *static_cast<std::uint32_t*>(invocation_state);
    // Instruction observers use the same value-initialized, invocation-local state contract as typed observers.
    if (invocation != 0) {
        ++state.state_mismatches;
    }
#if defined(_M_X64)
    invocation = static_cast<std::uint32_t>(cpu->rax);
#else
    invocation = cpu->eax;
#endif
    SetLastError(0x4444);
}

void FC_CALL instruction_after(void* context, const FC_CpuContext* cpu, const void* invocation_state) noexcept {
    auto& state = *static_cast<PatchCallbackState*>(context);
    ++state.after_calls;
#if defined(_M_X64)
    const auto result = static_cast<std::uint32_t>(cpu->rax);
#else
    const auto result = cpu->eax;
#endif
    if (result != 7 || *static_cast<const std::uint32_t*>(invocation_state) == 7) {
        ++state.state_mismatches;
    }
    SetLastError(0x5555);
}

FC_CallStatus FC_CALL fail_hook_builder(const FC_HookBuildInput*, const FC_ErrorSink* error) noexcept {
    set_callback_error(error, "Fixture builder rejected its entry", "Build fixture hook");
    return FC_CALL_FAILED;
}

// Submits the owner or observer shape selected by the script while keeping every participant on one known site.
[[nodiscard]] FC_CallStatus submit_shared_hook(PatchCallbackState& state, const FC_PlanSink& sink) {
    // Instruction participants use CpuContext callbacks and invocation-local state instead of a typed native call.
    if (state.script == LifecycleScript::InstructionHookOwner) {
        const FC_HookRequest request{.struct_size = sizeof(FC_HookRequest),
                                     .location = location(FC_LOCATION_CODE, 485),
                                     .kind = FC_HOOK_INSTRUCTION,
                                     .builder = fc::detail::instruction_hook_builder(),
                                     .context = &state,
                                     .callback = reinterpret_cast<std::uintptr_t>(&instruction_owner)};
        return sink.hook(sink.context, &request) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
    }
    if (state.script == LifecycleScript::InstructionHookObserver) {
        const FC_ObserverRequest request{.struct_size = sizeof(FC_ObserverRequest),
                                         .location = location(FC_LOCATION_CODE, 485),
                                         .kind = FC_HOOK_INSTRUCTION,
                                         .builder = fc::detail::instruction_hook_builder(),
                                         .context = &state,
                                         .before = reinterpret_cast<std::uintptr_t>(&instruction_before),
                                         .after = reinterpret_cast<std::uintptr_t>(&instruction_after),
                                         .state_size = sizeof(std::uint32_t),
                                         .state_alignment = alignof(std::uint32_t)};
        return sink.observe(sink.context, &request) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
    }

    // Every typed variant shares one signature; the script selects entry versus direct call placement and builder.
    auto native_call = fc::detail::native_call_storage<HookCall>();
    const auto builder =
        state.script == LifecycleScript::HookBrokenBuilder
            ? FC_HookBuilder{.build = &fail_hook_builder, .entry_size = fc::detail::kGeneratedHookEntrySize}
            : fc::detail::typed_hook_builder<HookCall>();
    const bool direct_owner = state.script == LifecycleScript::DirectHookOwner;
    const bool owner = state.script == LifecycleScript::HookOwner || direct_owner;
#if defined(_M_X64)
    constexpr std::uint32_t direct_call_rva = 404;
#else
    constexpr std::uint32_t direct_call_rva = 408;
#endif
    const auto hook_kind = direct_owner || state.script == LifecycleScript::DirectHookObserver
                               ? FC_HOOK_DIRECT_CALL_SITE
                               : FC_HOOK_FUNCTION_ENTRY;
    std::array<std::byte, 5> expected_hook_bytes{};
#if defined(_M_X64)
    expected_hook_bytes = {std::byte{0x8b}, std::byte{0xc1}, std::byte{0x03}, std::byte{0xc2}, std::byte{0xc3}};
#else
    expected_hook_bytes = {std::byte{0x8b}, std::byte{0x44}, std::byte{0x24}, std::byte{0x04}, std::byte{0x03}};
#endif
    const FC_Evidence hook_evidence =
        state.hook_evidence ? FC_Evidence{.kind = FC_EVIDENCE_EXACT_BYTES, .bytes = bytes(expected_hook_bytes)}
                            : FC_Evidence{};
    const auto hook_location = hook_kind == FC_HOOK_FUNCTION_ENTRY
                                   ? location(FC_LOCATION_FUNCTION, 320, hook_evidence)
                                   : location(FC_LOCATION_CODE, direct_call_rva, hook_evidence);
    // Owners receive Original binding storage, while observers retain paired before/after state on the same site.
    if (owner) {
        const FC_HookRequest request{.struct_size = sizeof(FC_HookRequest),
                                     .location = hook_location,
                                     .kind = hook_kind,
                                     .native_call = &native_call.call,
                                     .builder = builder,
                                     .context = &state,
                                     .callback = reinterpret_cast<std::uintptr_t>(&hook_owner),
                                     .original_context = &state,
                                     .bind_original = &bind_hook_original};
        return sink.hook(sink.context, &request) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
    }
    const FC_ObserverRequest request{.struct_size = sizeof(FC_ObserverRequest),
                                     .location = hook_location,
                                     .kind = hook_kind,
                                     .native_call = &native_call.call,
                                     .builder = builder,
                                     .context = &state,
                                     .before = reinterpret_cast<std::uintptr_t>(&hook_before),
                                     .after = reinterpret_cast<std::uintptr_t>(&hook_after),
                                     .state_size = sizeof(std::int32_t),
                                     .state_alignment = alignof(std::int32_t)};
    return sink.observe(sink.context, &request) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
}

// These helpers submit the symbolic allocation and ordinary byte writes reused by several lifecycle scripts.
[[nodiscard]] FC_SubmitResult submit_allocation(PatchCallbackState& state, const FC_PlanSink& sink) {
    // Retain a recognizable word so the Prepare callback proves allocation initialization precedes author code.
    constexpr std::uint32_t kInitialValue = 0x1122'3344;
    const auto initial = std::as_bytes(std::span{&kInitialValue, 1});
    const FC_DataAllocationRequest request{.struct_size = sizeof(FC_DataAllocationRequest),
                                           .byte_size = initial.size(),
                                           .alignment = alignof(std::uint32_t),
                                           .initial_bytes = bytes(initial),
                                           .name = text("fixture native state")};
    return sink.allocate_data(sink.context, &request, &state.data_handle);
}

[[nodiscard]] bool submit_write(const FC_PlanSink& sink, FC_LocationKind kind, std::uint32_t rva,
                                std::span<const std::byte> replacement, FC_Evidence evidence = {}) {
    const FC_WriteRequest request{.struct_size = sizeof(FC_WriteRequest),
                                  .location = location(kind, rva, evidence),
                                  .kind = FC_WRITE_BYTES,
                                  .bytes = bytes(replacement)};
    return sink.write(sink.context, &request) == FC_SUBMIT_ACCEPTED;
}

// One plan exercises every fundamental byte encoder so their addresses, resources, and publication share a lifecycle.
FC_CallStatus submit_mutation_matrix(PatchCallbackState& state, const FC_PlanSink& sink) {
    const auto data_begin = static_cast<std::uint32_t>(state.page_size);
    if (submit_allocation(state, sink) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }

    // Pointer-width and rel32 encodings resolve the same symbolic allocation after native placement.
    const FC_WriteRequest pointer_write{.struct_size = sizeof(FC_WriteRequest),
                                        .location = location(FC_LOCATION_DATA, data_begin + 128),
                                        .kind = FC_WRITE_POINTER,
                                        .target = {.kind = FC_ADDRESS_DATA, .data = state.data_handle}};
    const FC_WriteRequest relative_write{.struct_size = sizeof(FC_WriteRequest),
                                         .location = location(FC_LOCATION_DATA, data_begin + 136),
                                         .kind = FC_WRITE_REL32,
                                         .target = {.kind = FC_ADDRESS_DATA, .data = state.data_handle}};
    if (sink.write(sink.context, &pointer_write) != FC_SUBMIT_ACCEPTED ||
        sink.write(sink.context, &relative_write) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }

    const std::array expected{std::byte{0x10}};
    const std::array replacement{std::byte{0x20}};
    const FC_Evidence exact{.kind = FC_EVIDENCE_EXACT_BYTES, .bytes = bytes(expected)};
    if (!submit_write(sink, FC_LOCATION_DATA, data_begin + 144, replacement, exact)) {
        return FC_CALL_FAILED;
    }

    // Executable mutations cover neutral bytes, an external function needing an x64 relay, and an image-local jump.
    const FC_NopRequest nop{
        .struct_size = sizeof(FC_NopRequest), .location = location(FC_LOCATION_CODE, 64), .size = 3};
    const FC_AddressTarget plugin_function{.kind = FC_ADDRESS_PLUGIN_FUNCTION,
                                           .plugin_function = reinterpret_cast<std::uintptr_t>(&branch_destination)};
    const FC_WriteRequest call{.struct_size = sizeof(FC_WriteRequest),
                               .location = location(FC_LOCATION_CODE, 80),
                               .kind = FC_WRITE_CALL,
                               .target = plugin_function};
    const FC_WriteRequest jump{.struct_size = sizeof(FC_WriteRequest),
                               .location = location(FC_LOCATION_CODE, 88),
                               .kind = FC_WRITE_JUMP,
                               .target = {.kind = FC_ADDRESS_IMAGE, .image_rva = 256}};
    if (sink.nop(sink.context, &nop) != FC_SUBMIT_ACCEPTED || sink.write(sink.context, &call) != FC_SUBMIT_ACCEPTED ||
        sink.write(sink.context, &jump) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }

    const FC_RedirectRequest redirect{.struct_size = sizeof(FC_RedirectRequest),
                                      .location = location(FC_LOCATION_CODE, 96),
                                      .kind = FC_REDIRECT_CALL,
                                      .target = plugin_function};
    if (sink.redirect(sink.context, &redirect, &state.original_target) != FC_SUBMIT_ACCEPTED) {
        return FC_CALL_FAILED;
    }

    // The read requirement proves evidence and resolved addresses survive until revalidation during the Prepare phase.
    const std::array required_byte{std::byte{0x10}};
    const FC_Evidence required_evidence{.kind = FC_EVIDENCE_EXACT_BYTES, .bytes = bytes(required_byte)};
    const FC_RequireRequest require{.struct_size = sizeof(FC_RequireRequest),
                                    .location = location(FC_LOCATION_DATA, data_begin + 160, required_evidence),
                                    .size = 1,
                                    .alignment = 1,
                                    .writable = FC_FALSE};
    return sink.require(sink.context, &require, &state.required_address) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK
                                                                                               : FC_CALL_FAILED;
}

// The native Plan callback selects one controlled operation set while every script uses the production plan sink.
FC_CallStatus FC_CALL plan_patch(void*, FC_PatchHandle patch, const FC_PlanContext*, const FC_PlanSink* sink,
                                 const FC_ErrorSink*) {
    // Each script produces a minimal patch plan whose later callback mutation isolates one lifecycle rule.
    auto& state = *static_cast<PatchCallbackState*>(patch);
    const auto data_begin = static_cast<std::uint32_t>(state.page_size);
    if (state.script == LifecycleScript::Empty || state.script == LifecycleScript::PrepareFailure) {
        return FC_CALL_OK;
    }
    if (state.script == LifecycleScript::MutationMatrix) {
        return submit_mutation_matrix(state, *sink);
    }
    if (state.script == LifecycleScript::HookOwner || state.script == LifecycleScript::HookObserver ||
        state.script == LifecycleScript::HookBrokenBuilder || state.script == LifecycleScript::DirectHookOwner ||
        state.script == LifecycleScript::DirectHookObserver || state.script == LifecycleScript::InstructionHookOwner ||
        state.script == LifecycleScript::InstructionHookObserver) {
        return submit_shared_hook(state, *sink);
    }
    if (state.script == LifecycleScript::RequiredStale) {
        const std::array expected{std::byte{0x10}};
        const FC_Evidence evidence{.kind = FC_EVIDENCE_EXACT_BYTES, .bytes = bytes(expected)};
        const FC_RequireRequest request{.struct_size = sizeof(FC_RequireRequest),
                                        .location = location(FC_LOCATION_DATA, data_begin + 160, evidence),
                                        .size = 1,
                                        .alignment = 1,
                                        .writable = FC_FALSE};
        return sink->require(sink->context, &request, &state.required_address) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK
                                                                                                     : FC_CALL_FAILED;
    }
    if (state.script == LifecycleScript::CodeAccessStale) {
        const FC_RequireRequest request{.struct_size = sizeof(FC_RequireRequest),
                                        .location = location(FC_LOCATION_CODE, 64),
                                        .size = 1,
                                        .alignment = 1,
                                        .writable = FC_FALSE};
        return sink->require(sink->context, &request, &state.required_address) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK
                                                                                                     : FC_CALL_FAILED;
    }
    if (state.script == LifecycleScript::CodeAccessMutation) {
        const FC_NopRequest request{
            .struct_size = sizeof(FC_NopRequest), .location = location(FC_LOCATION_CODE, 64), .size = 1};
        return sink->nop(sink->context, &request) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
    }
    if (state.script == LifecycleScript::RetainedFailure) {
        if (submit_allocation(state, *sink) != FC_SUBMIT_ACCEPTED) {
            return FC_CALL_FAILED;
        }
        // This read claim verifies that retained exposure promotes every original access to an exclusive blocker.
        const FC_RequireRequest retained_read{.struct_size = sizeof(FC_RequireRequest),
                                              .location = location(FC_LOCATION_DATA, data_begin + 80),
                                              .size = 1,
                                              .alignment = 1,
                                              .writable = FC_FALSE};
        if (sink->require(sink->context, &retained_read, &state.required_address) != FC_SUBMIT_ACCEPTED) {
            return FC_CALL_FAILED;
        }
        const std::array first{std::byte{0x41}};
        const std::array second{std::byte{0x42}};
        const auto second_data_begin = static_cast<std::uint32_t>(state.page_size * 2);
        return submit_write(*sink, FC_LOCATION_DATA, data_begin + 32, first) &&
                       submit_write(*sink, FC_LOCATION_DATA, second_data_begin + 32, second)
                   ? FC_CALL_OK
                   : FC_CALL_FAILED;
    }

    const std::array replacement{std::byte{0x20}};
    FC_Evidence evidence{};
    std::array<std::byte, 1> expected{std::byte{0x10}};
    if (state.script == LifecycleScript::EvidenceMutation) {
        evidence = {.kind = FC_EVIDENCE_EXACT_BYTES, .bytes = bytes(expected)};
    }
    return submit_write(*sink, FC_LOCATION_DATA, data_begin + 144, replacement, evidence) ? FC_CALL_OK : FC_CALL_FAILED;
}

// Cold lifecycle fixtures expose Prepare callback failure, later input mutation, activation, and an Update callback.
FC_CallStatus FC_CALL prepare_patch(void*, FC_PatchHandle patch, const FC_PrepareContext* context,
                                    const FC_ErrorSink* error) {
    auto& state = *static_cast<PatchCallbackState*>(patch);
    ++state.prepare_count;
    if (state.script == LifecycleScript::PrepareFailure) {
        set_callback_error(error, "The fixture's Prepare callback rejected its resources", "Prepare fixture patch");
        return FC_CALL_FAILED;
    }
    if (state.script == LifecycleScript::IgnoredResolveFailure) {
        // A hand-written callback deliberately ignores rejection; the core must still poison the Prepare invocation.
        std::uintptr_t address = 1;
        std::uint64_t byte_size = 1;
        const auto result = context->resolve_data(context->context, FC_INVALID_DATA_HANDLE, &address, &byte_size);
        state.rejected_resolution_cleared_outputs = result == FC_FALSE && address == 0 && byte_size == 0;
        return FC_CALL_OK;
    }
    if (state.script == LifecycleScript::HookOwner || state.script == LifecycleScript::DirectHookOwner) {
        state.original_bound_during_prepare = state.hook_original != 0;
    }
    if (state.data_handle != FC_INVALID_DATA_HANDLE) {
        // Resolve through the attempt-scoped ABI rather than retaining an address from planning.
        std::uint64_t byte_size{};
        if (context->resolve_data(context->context, state.data_handle, &state.data_address, &byte_size) != FC_TRUE ||
            byte_size != sizeof(std::uint32_t)) {
            return FC_CALL_FAILED;
        }
    }
    if (state.script == LifecycleScript::MutationMatrix) {
        // Verify allocation initialization and redirect decoding before changing plugin-owned native state.
        if (*reinterpret_cast<const std::uint32_t*>(state.data_address) != 0x1122'3344 ||
            state.original_target != state.image_base + 256) {
            return FC_CALL_FAILED;
        }
        *reinterpret_cast<std::uint32_t*>(state.data_address) = 0x5566'7788;
    }
    if (state.script == LifecycleScript::EvidenceMutation || state.script == LifecycleScript::RawMutation) {
        // The same drift is rejected only when the patch plan supplied evidence for the destination preimage.
        *reinterpret_cast<std::byte*>(state.image_base + state.page_size + 144) = std::byte{0x30};
    }
    if (state.script == LifecycleScript::CodeAccessMutation) {
        DWORD previous{};
        if (!VirtualProtect(reinterpret_cast<void*>(state.image_base), state.page_size, PAGE_READWRITE, &previous)) {
            return FC_CALL_FAILED;
        }
    }
    return FC_CALL_OK;
}

void FC_CALL activate_patch(void*, FC_PatchHandle patch, const FC_ActivateContext*) {
    ++static_cast<PatchCallbackState*>(patch)->activate_count;
}

void FC_CALL update_patch(void*, FC_PatchHandle patch, const FC_UpdateContext*) noexcept {
    ++static_cast<PatchCallbackState*>(patch)->update_count;
}

// Plugin catalog helpers give each script the same support so tests vary lifecycle behavior, not admission.
[[nodiscard]] fc::catalog::PatchDefinitionRecord patch(std::string id, PatchCallbackState& state,
                                                       std::vector<std::string> dependencies = {},
                                                       FC_FailurePolicy policy = FC_FAILURE_CONTINUE) {
    fc::catalog::SupportDefinitionRecord support{.layouts = kTestLayout,
                                                 .roles = FC_HOST_ROLE_CLIENT,
                                                 .image = FC_IMAGE_GAME,
                                                 .callbacks = {.context = &state,
                                                               .create = &create_patch,
                                                               .destroy = &destroy_patch,
                                                               .plan = &plan_patch,
                                                               .prepare = &prepare_patch,
                                                               .activate = &activate_patch,
                                                               .update = &update_patch},
                                                 .depends_on = dependencies,
                                                 .failure_policy = FC_FAILURE_INHERIT};
    return {.id = std::move(id),
            .name = "Lifecycle fixture",
            .enabled = FC_TRUE,
            .failure_policy = policy,
            .supports = {std::move(support)},
            .selected_support = 0};
}

[[nodiscard]] fc::catalog::Catalog catalog(std::vector<fc::catalog::PatchDefinitionRecord> patches) {
    auto owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&plan_patch));
    REQUIRE(owner.has_value());
    fc::catalog::PluginRecord plugin;
    plugin.sdk_revision = FC_SDK_REVISION;
    plugin.code_owner = std::move(*owner);
    plugin.definition.id = "PatchingFixture";
    plugin.definition.patches = std::move(patches);
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(plugin));
    return fc::catalog::Catalog{std::move(plugins)};
}

[[nodiscard]] fc::config::ConfigurationSnapshot configuration(const fc::catalog::Catalog& source) {
    fc::config::ConfigurationSnapshot result;
    for (const auto& plugin : source.plugins()) {
        fc::config::PluginConfiguration configured{.plugin_id = plugin.definition.id};
        configured.patches.resize(plugin.definition.patches.size());
        configured.groups.resize(plugin.definition.groups.size());
        result.plugins.push_back(std::move(configured));
    }
    return result;
}

// Couples a native image backed by real pages with its system page size so tests can change Windows protection.
struct PrivateTarget {
    fc::targets::RecognizedTarget target;
    std::size_t page_size{};
};

[[nodiscard]] PrivateTarget private_target() {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto page_size = static_cast<std::size_t>(system.dwPageSize);
    // Separate executable and writable pages make semantic access changes observable during revalidation.
    auto game_bytes = std::vector<std::byte>(page_size * 3, std::byte{});
    std::fill_n(game_bytes.begin(), page_size, std::byte{0x90});
    // Hand-authored function, call, and instruction sites give the production hook paths deterministic native code.
#if defined(_M_X64)
    const std::array hook_target{std::byte{0x8b}, std::byte{0xc1}, std::byte{0x03}, std::byte{0xc2}, std::byte{0xc3}};
#else
    const std::array hook_target{std::byte{0x8b}, std::byte{0x44}, std::byte{0x24}, std::byte{0x04}, std::byte{0x03},
                                 std::byte{0x44}, std::byte{0x24}, std::byte{0x08}, std::byte{0xc3}};
#endif
    std::copy(hook_target.begin(), hook_target.end(), game_bytes.begin() + 320);

#if defined(_M_X64)
    std::array direct_caller{std::byte{0x48}, std::byte{0x83}, std::byte{0xec}, std::byte{0x28}, std::byte{0xe8},
                             std::byte{},     std::byte{},     std::byte{},     std::byte{},     std::byte{0x48},
                             std::byte{0x83}, std::byte{0xc4}, std::byte{0x28}, std::byte{0x83}, std::byte{0xc0},
                             std::byte{0x01}, std::byte{0xc3}};
    constexpr std::uint32_t direct_call_rva = 404;
#else
    std::array direct_caller{std::byte{0xff}, std::byte{0x74}, std::byte{0x24}, std::byte{0x08}, std::byte{0xff},
                             std::byte{0x74}, std::byte{0x24}, std::byte{0x08}, std::byte{0xe8}, std::byte{},
                             std::byte{},     std::byte{},     std::byte{},     std::byte{0x83}, std::byte{0xc4},
                             std::byte{0x08}, std::byte{0x83}, std::byte{0xc0}, std::byte{0x01}, std::byte{0xc3}};
    constexpr std::uint32_t direct_call_rva = 408;
#endif
    const auto direct_displacement = std::int32_t{320} - static_cast<std::int32_t>(direct_call_rva + 5);
    std::memcpy(direct_caller.data() + (direct_call_rva - 400) + 1, &direct_displacement, sizeof(direct_displacement));
    std::copy(direct_caller.begin(), direct_caller.end(), game_bytes.begin() + 400);

    // The instruction site follows MOV EAX,1 so a context owner can change the returned value before RET.
    constexpr std::array instruction_target{std::byte{0xb8}, std::byte{0x01}, std::byte{},    std::byte{},
                                            std::byte{},     std::byte{0x90}, std::byte{0xc3}};
    std::copy(instruction_target.begin(), instruction_target.end(), game_bytes.begin() + 480);
    game_bytes[page_size + 144] = std::byte{0x10};
    game_bytes[page_size + 160] = std::byte{0x10};
    const std::int32_t original = 256 - (96 + 5);
    game_bytes[96] = std::byte{0xe8};
    std::memcpy(game_bytes.data() + 97, &original, sizeof(original));

#if defined(_M_X64)
    constexpr std::string_view game_profile = "ClassicCollection_Game_66702CD2";
#else
    constexpr std::string_view game_profile = "SteamRetail_Game_59EDE353";
#endif
    auto game = fc::targets::OwnedImage::private_native(
        {FC_IMAGE_GAME, game_profile, 0, game_bytes.size()}, std::move(game_bytes),
        {{static_cast<std::uint32_t>(page_size), static_cast<std::uint32_t>(page_size * 2)}},
        {{0, static_cast<std::uint32_t>(page_size * 3)}}, {{0, static_cast<std::uint32_t>(page_size)}});
    REQUIRE(game.has_value());
    std::vector<fc::targets::OwnedImage> images;
#if defined(_M_X64)
    auto bootstrap_bytes = std::vector<std::byte>(256, std::byte{0x90});
    images.push_back(fc::targets::OwnedImage::mapped(
        {FC_IMAGE_BOOTSTRAP, "ClassicCollection_Bootstrap_66702CD7", 0, bootstrap_bytes.size()},
        std::move(bootstrap_bytes), {}, {{0, 256}}, {{0, 256}}));
#endif
    images.push_back(std::move(*game));
    auto recognized = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(images));
    REQUIRE(recognized.has_value());
    const auto* view = recognized->find(FC_IMAGE_GAME);
    REQUIRE(view != nullptr);
    DWORD previous{};
    REQUIRE(VirtualProtect(reinterpret_cast<void*>(view->info().base), page_size, PAGE_EXECUTE_READ, &previous));
    return {std::move(*recognized), page_size};
}

// Resolves one native target and fixture catalog while preserving callback state outside the runtime owner.
[[nodiscard]] std::unique_ptr<fc::runtime::PatchRuntimeState>
runtime_with(std::vector<fc::catalog::PatchDefinitionRecord> patches, std::span<PatchCallbackState> states) {
    // Build the resolution owner and publish image facts before the Create and Plan callbacks execute.
    auto native_target = private_target();
    auto runtime =
        std::make_unique<fc::runtime::PatchRuntimeState>(std::move(native_target.target), catalog(std::move(patches)));
    const auto* image = runtime->target.find(FC_IMAGE_GAME);
    REQUIRE(image != nullptr);
    for (auto& state : states) {
        state.image_base = image->info().base;
        state.page_size = native_target.page_size;
    }
    auto configured = configuration(runtime->catalog);
    runtime->patches = fc::planning::resolve_patches({runtime->catalog, runtime->target, configured});
    return runtime;
}

[[nodiscard]] fc::planning::PatchState state(const fc::runtime::PatchRuntimeState& runtime, std::string_view id) {
    const auto index = runtime.catalog.find_patch(id);
    REQUIRE(index.has_value());
    return runtime.patches.record(*index).state;
}

// Decodes the fixture's direct CALL so tests can verify semantic restoration rather than compare raw displacements.
[[nodiscard]] std::uintptr_t direct_target(const fc::targets::ImageView& image, std::uint32_t rva) {
    std::array<std::byte, 5> instruction{};
    REQUIRE(image.read({rva}, instruction).has_value());
    std::int32_t displacement{};
    std::memcpy(&displacement, instruction.data() + 1, sizeof(displacement));
    auto target = fc::planning::decode_rel32_target(image.info().base + rva + 5, displacement, kTestArchitecture);
    REQUIRE(target.has_value());
    return *target;
}

// Identifies the two transaction ranges and call sequence used to inject a deterministic residual rollback.
struct FakeWriterState {
    std::uintptr_t first{};
    std::uintptr_t second{};
    std::size_t calls{};
};

// Delegates real writes except a failure on the second range, allowing the first range to roll back normally.
struct FailSecondWrite {
    fc::patching::NativeMemoryWriter system{fc::patching::system_memory_writer()};
    std::uintptr_t second{};
    bool injected{};
};

// Models direct call hook commit outcomes; write exposure controls ownership of a previously absent site.
enum class DirectHookWriteFailure {
    NoExposure,
    ExposedThenRestored,
};

struct DirectHookWriterState {
    fc::patching::NativeMemoryWriter system{fc::patching::system_memory_writer()};
    std::uintptr_t site{};
    DirectHookWriteFailure failure{};
    std::size_t site_calls{};
};

// Records vendor-managed publication and restoration without reproducing PatchTransaction's ordering logic.
struct NativeEffectState {
    std::vector<int>* events{};
    int identity{};
    bool fail_apply{};
};

// A real private target receives the first write and rollback; only the platform failure point is deterministic.
std::expected<void, fc::patching::NativeWriteFailure> fail_second_write(void* context, std::uintptr_t address,
                                                                        std::span<const std::byte> replacement) {
    auto& state = *static_cast<FailSecondWrite*>(context);
    if (address == state.second && !state.injected) {
        state.injected = true;
        return std::unexpected(fc::patching::NativeWriteFailure{"Injected native write failure"});
    }
    return state.system.write(state.system.context, address, replacement);
}

// Fails the first direct call site publication; the exposed case writes bytes that rollback must restore.
std::expected<void, fc::patching::NativeWriteFailure> fail_direct_hook_write(void* context, std::uintptr_t address,
                                                                             std::span<const std::byte> replacement) {
    auto& state = *static_cast<DirectHookWriterState*>(context);
    if (address != state.site) {
        return state.system.write(state.system.context, address, replacement);
    }
    ++state.site_calls;
    if (state.site_calls != 1) {
        return state.system.write(state.system.context, address, replacement);
    }
    if (state.failure == DirectHookWriteFailure::NoExposure) {
        return std::unexpected(fc::patching::NativeWriteFailure{"Injected rejection while writing a direct call hook"});
    }
    auto published = state.system.write(state.system.context, address, replacement);
    if (!published) {
        return published;
    }
    return std::unexpected(
        fc::patching::NativeWriteFailure{"Injected failure after exposing a direct call hook", true});
}

// Positive and negative event IDs expose effect ordering while the second publisher supplies the controlled failure.
std::expected<void, fc::patching::NativeWriteFailure> apply_native_effect(void* context) {
    auto& state = *static_cast<NativeEffectState*>(context);
    state.events->push_back(state.identity);
    if (state.fail_apply) {
        return std::unexpected(fc::patching::NativeWriteFailure{"Injected failure while publishing a native effect"});
    }
    return {};
}

std::expected<void, fc::patching::NativeWriteFailure> restore_native_effect(void* context) {
    auto& state = *static_cast<NativeEffectState*>(context);
    state.events->push_back(-state.identity);
    return {};
}

// The fake controls only platform outcomes; PatchTransaction still owns ordering, preimages, and reverse unwind.
std::expected<void, fc::patching::NativeWriteFailure> fake_residual_write(void* context, std::uintptr_t address,
                                                                          std::span<const std::byte> replacement) {
    auto& state = *static_cast<FakeWriterState*>(context);
    ++state.calls;
    if (address == state.first && state.calls == 1) {
        std::memcpy(reinterpret_cast<void*>(address), replacement.data(), replacement.size());
        return {};
    }
    if (address == state.second) {
        return std::unexpected(fc::patching::NativeWriteFailure{"Injected failure during the second operation"});
    }
    return std::unexpected(fc::patching::NativeWriteFailure{"Injected rollback failure", true, true});
}

// Each matrix row isolates native exposure and retained process-lifetime ownership in a disposable process.
enum class InstallationFailureCase {
    BeforeExposure,
    RestoredExposure,
    ResidualExposure,
};

void verify_installation_failure_case(InstallationFailureCase failure_case) {
    std::array<PatchCallbackState, 4> callbacks;
    callbacks[0].script = failure_case == InstallationFailureCase::BeforeExposure ? LifecycleScript::PrepareFailure
                                                                                  : LifecycleScript::RetainedFailure;
    {
        auto runtime = runtime_with(
            {patch("Provider", callbacks[0]), patch("DirectConsumer", callbacks[1], {"Provider"}),
             patch("TransitiveConsumer", callbacks[2], {"DirectConsumer"}), patch("Unrelated", callbacks[3])},
            callbacks);
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
        const auto first_address = callbacks[0].image_base + callbacks[0].page_size + 32;
        const auto second_address = callbacks[0].image_base + callbacks[0].page_size * 2 + 32;
        const auto original_first = *reinterpret_cast<const std::byte*>(first_address);

        fc::runtime::InstallationResult result{};
        if (failure_case == InstallationFailureCase::BeforeExposure) {
            result = fc::runtime::install_ready_patches(plan, *runtime);
        } else if (failure_case == InstallationFailureCase::RestoredExposure) {
            FailSecondWrite failure{.second = second_address};
            result =
                fc::runtime::install_ready_patches(plan, *runtime, {.memory_writer = {&failure, &fail_second_write}});
        } else {
            FakeWriterState failure{first_address, second_address};
            result =
                fc::runtime::install_ready_patches(plan, *runtime, {.memory_writer = {&failure, &fake_residual_write}});
        }

        // Every failure row prunes the complete consumer chain while unrelated installation remains eligible.
        CHECK(result == fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "Provider") == fc::planning::PatchState::Failed);
        CHECK(state(*runtime, "DirectConsumer") == fc::planning::PatchState::Skipped);
        CHECK(state(*runtime, "TransitiveConsumer") == fc::planning::PatchState::Skipped);
        CHECK(state(*runtime, "Unrelated") == fc::planning::PatchState::Installed);
        CHECK(callbacks[1].prepare_count == 0);
        CHECK(callbacks[2].prepare_count == 0);
        CHECK(callbacks[3].activate_count == 1);

        if (failure_case == InstallationFailureCase::BeforeExposure) {
            CHECK(runtime->retained_failures.empty());
            CHECK(callbacks[0].destroy_count == 1);
        } else {
            REQUIRE(runtime->retained_failures.size() == 1);
            const auto expected = failure_case == InstallationFailureCase::RestoredExposure
                                      ? fc::patching::RollbackResult::Restored
                                      : fc::patching::RollbackResult::Residual;
            CHECK(runtime->retained_failures.front().rollback == expected);
            CHECK(callbacks[0].destroy_count == 0);
            CHECK(callbacks[0].data_address != 0);
            CHECK(runtime->blocked_claims.size() == 3);
            if (failure_case == InstallationFailureCase::RestoredExposure) {
                CHECK(byte_value(*reinterpret_cast<const std::byte*>(first_address)) == byte_value(original_first));
            } else {
                CHECK(byte_value(*reinterpret_cast<const std::byte*>(first_address)) == 0x41);
            }
        }
    }
    // Retained instances are destroyed only when the isolated runtime releases their still-live native resources.
    CHECK(callbacks[0].destroy_count == 1);
    if (failure_case != InstallationFailureCase::BeforeExposure) {
        CHECK(callbacks[0].allocation_live_during_destroy);
    }
}

struct ChildProcessResult {
    bool launched{};
    DWORD exit_code{};
};

// Runs one hidden Catch2 row in a fresh copy of this executable so residual native state cannot leak into another row.
[[nodiscard]] ChildProcessResult run_installation_failure_child(std::string_view test_name) {
    std::array<wchar_t, 32'768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= static_cast<DWORD>(executable.size())) {
        return {};
    }
    const std::wstring wide_name{test_name.begin(), test_name.end()};
    std::wstring command =
        L"\"" + std::wstring{executable.data(), length} + L"\" \"" + wide_name + L"\" --reporter compact";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                       &startup, &process) == 0) {
        return {};
    }
    const auto wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code{};
    const bool completed = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return {.launched = completed, .exit_code = exit_code};
}

} // namespace

TEST_CASE("installation failure child: before exposure", "[.][installation-failure-child]") {
    verify_installation_failure_case(InstallationFailureCase::BeforeExposure);
}

TEST_CASE("installation failure child: restored exposure", "[.][installation-failure-child]") {
    verify_installation_failure_case(InstallationFailureCase::RestoredExposure);
}

TEST_CASE("installation failure child: residual exposure", "[.][installation-failure-child]") {
    verify_installation_failure_case(InstallationFailureCase::ResidualExposure);
}

TEST_CASE("installation failures isolate rollback ownership, consumer pruning, and unrelated continuation") {
    constexpr std::array cases{
        std::string_view{"installation failure child: before exposure"},
        std::string_view{"installation failure child: restored exposure"},
        std::string_view{"installation failure child: residual exposure"},
    };
    for (const auto test_name : cases) {
        INFO("Child test: " << test_name);
        const auto result = run_installation_failure_child(test_name);
        REQUIRE(result.launched);
        CHECK(result.exit_code == 0);
    }
}

TEST_CASE("symbolic storage and every fundamental mutation publish through one lifecycle") {
    PatchCallbackState callback{.script = LifecycleScript::MutationMatrix};
    {
        auto runtime = runtime_with({patch("MutationMatrix", callback)}, std::span{&callback, 1});
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
        REQUIRE(plan.installation_order.size() == 1);
        const auto planned_claims = runtime->patches.record(plan.installation_order.front()).plan.claims.size();

        CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "MutationMatrix") == fc::planning::PatchState::Installed);
        CHECK(callback.prepare_count == 1);
        CHECK(callback.activate_count == 1);
        CHECK(callback.destroy_count == 0);
        CHECK(*reinterpret_cast<const std::uint32_t*>(callback.data_address) == 0x5566'7788);

        // Decode published bytes back to semantic destinations instead of depending on platform address placement.
        const auto* image = runtime->target.find(FC_IMAGE_GAME);
        REQUIRE(image != nullptr);
        const auto data_begin = callback.page_size;
        CHECK(*reinterpret_cast<const std::uintptr_t*>(image->info().base + data_begin + 128) == callback.data_address);
        std::int32_t relative{};
        std::memcpy(&relative, reinterpret_cast<const void*>(image->info().base + data_begin + 136), sizeof(relative));
        const auto relative_target =
            fc::planning::decode_rel32_target(image->info().base + data_begin + 140, relative, kTestArchitecture);
        REQUIRE(relative_target.has_value());
        CHECK(*relative_target == callback.data_address);
        CHECK(byte_value(*reinterpret_cast<const std::byte*>(image->info().base + data_begin + 144)) == 0x20);
        CHECK(byte_value(*reinterpret_cast<const std::byte*>(image->info().base + 64)) == 0x90);
        CHECK(direct_target(*image, 88) == image->info().base + 256);

        // A distant x64 callback is reached through an owned relay; x86 may encode it directly.
        for (const auto rva : {80U, 96U}) {
            const auto destination = direct_target(*image, rva);
            if (destination != reinterpret_cast<std::uintptr_t>(&branch_destination)) {
                std::array<std::byte, 14> relay{};
                SIZE_T read{};
                REQUIRE(ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(destination), relay.data(),
                                          relay.size(), &read));
                REQUIRE(read == relay.size());
                CHECK(byte_value(relay[0]) == 0xff);
                CHECK(byte_value(relay[1]) == 0x25);
                std::uintptr_t absolute{};
                std::memcpy(&absolute, relay.data() + 6, sizeof(absolute));
                CHECK(absolute == reinterpret_cast<std::uintptr_t>(&branch_destination));
            }
        }
        CHECK(runtime->installed_patches.size() == 1);
        CHECK(runtime->installed_claims.size() == planned_claims);
    }
    // Runtime teardown destroys the instance while its process-lifetime allocation is still owned and addressable.
    CHECK(callback.destroy_count == 1);
    CHECK(callback.allocation_live_during_destroy);
}

TEST_CASE("successful publication builds one Update callback dispatch list ordered by patch ID") {
    std::array states{PatchCallbackState{}, PatchCallbackState{}};
    auto runtime = runtime_with({patch("ZuluUpdate", states[0]), patch("AlphaUpdate", states[1])}, states);
    const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
    REQUIRE(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
    REQUIRE(runtime->update_order.size() == 2);

    // Retained indexes, rather than copied callback tuples, remain valid as records for later images join the vector.
    const auto first = runtime->installed_patches[runtime->update_order[0]].patch;
    const auto second = runtime->installed_patches[runtime->update_order[1]].patch;
    CHECK(runtime->catalog.patch(first).id == "AlphaUpdate");
    CHECK(runtime->catalog.patch(second).id == "ZuluUpdate");
    for (const auto index : runtime->update_order) {
        runtime->installed_patches[index].instance.update(nullptr);
    }
    CHECK(states[1].update_count == 1);
    CHECK(states[0].update_count == 1);
}

TEST_CASE("the production registry installs one physical function hook and publishes late participants") {
    // Stable IDs make the observer create the absent site before the owner joins its installed snapshot stream.
    std::array<PatchCallbackState, 2> callbacks{
        {{.script = LifecycleScript::HookObserver}, {.script = LifecycleScript::HookOwner}}};
    auto runtime = runtime_with({patch("AObserver", callbacks[0]), patch("ZOwner", callbacks[1])}, callbacks);
    const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

    REQUIRE(plan.hook_aggregates.size() == 1);
    CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
    CHECK(state(*runtime, "AObserver") == fc::planning::PatchState::Installed);
    CHECK(state(*runtime, "ZOwner") == fc::planning::PatchState::Installed);
    CHECK(callbacks[1].original_bound_during_prepare);

    const auto hooked = reinterpret_cast<HookCall>(callbacks[0].image_base + 320);
    SetLastError(0x5151);
    CHECK(hooked(2, 3) == 10);
    CHECK(callbacks[0].before_calls == 1);
    CHECK(callbacks[0].after_calls == 1);
    CHECK(callbacks[0].state_mismatches == 0);
    CHECK(callbacks[1].hook_calls == 1);
    CHECK(GetLastError() == 0x5151);

    const auto sites = runtime->hooks.installed_sites();
    REQUIRE(sites.size() == 1);
    CHECK(sites.front().has_owner);
    CHECK(sites.front().observer_count == 1);
    CHECK(sites.front().state_size == sizeof(std::int32_t));
    CHECK(sites.front().original_bytes.size() == sites.front().overwrite_size);
    CHECK(runtime->installed_claims.size() == 1);
}

TEST_CASE("a later validation pass joins installed code through its logical original view") {
    std::array<PatchCallbackState, 2> callbacks{
        {{.script = LifecycleScript::HookOwner}, {.script = LifecycleScript::HookObserver, .hook_evidence = true}}};
    auto runtime = runtime_with({patch("AOwner", callbacks[0]), patch("ZLateObserver", callbacks[1])}, callbacks);
    const auto late = runtime->catalog.find_patch("ZLateObserver");
    REQUIRE(late.has_value());

    // The first pass contains only startup work and detours the site while the late participant remains idle.
    runtime->patches.record(*late).state = fc::planning::PatchState::Disabled;
    const auto first_plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
    REQUIRE(fc::runtime::install_ready_patches(first_plan, *runtime) == fc::runtime::InstallationResult::Completed);
    REQUIRE(runtime->hooks.installed_sites().size() == 1);
    const auto first_entry = runtime->hooks.installed_sites().front();
    CHECK(first_entry.has_owner);
    CHECK(first_entry.observer_count == 0);

    // A distinct late pass supplies the original function bytes as evidence even though live memory now holds a
    // SafetyHook detour. The retained preimage must serve planning and both installer revalidation passes.
    runtime->patches.record(*late).state = fc::planning::PatchState::Pending;
    const auto late_plan =
        fc::planning::build_installation_plan(runtime->target, runtime->patches, runtime->validation_baseline());
    REQUIRE(late_plan.installation_order.size() == 1);
    CHECK(fc::runtime::install_ready_patches(late_plan, *runtime) == fc::runtime::InstallationResult::Completed);
    CHECK(state(*runtime, "ZLateObserver") == fc::planning::PatchState::Installed);

    const auto sites = runtime->hooks.installed_sites();
    REQUIRE(sites.size() == 1);
    CHECK(sites.front().overwrite_size == first_entry.overwrite_size);
    CHECK(sites.front().observer_count == 1);
    const auto hooked = reinterpret_cast<HookCall>(callbacks[0].image_base + 320);
    CHECK(hooked(2, 3) == 10);
    CHECK(callbacks[1].before_calls == 1);
    CHECK(callbacks[1].after_calls == 1);
}

TEST_CASE("typed direct call and instruction sites use their distinct production placement paths") {
    SECTION("a direct call snapshot changes only the selected caller") {
        // The caller should route through the shared snapshot while direct invocation of its callee remains untouched.
        std::array<PatchCallbackState, 2> callbacks{
            {{.script = LifecycleScript::DirectHookObserver}, {.script = LifecycleScript::DirectHookOwner}}};
        auto runtime = runtime_with({patch("AObserver", callbacks[0]), patch("ZOwner", callbacks[1])}, callbacks);
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

        CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
        const auto caller = reinterpret_cast<HookCall>(callbacks[0].image_base + 400);
        const auto target = reinterpret_cast<HookCall>(callbacks[0].image_base + 320);
        CHECK(caller(2, 3) == 11);
        CHECK(target(2, 3) == 5);
        CHECK(callbacks[0].before_calls == 1);
        CHECK(callbacks[0].after_calls == 1);
        REQUIRE(runtime->hooks.installed_sites().size() == 1);
        CHECK(runtime->hooks.installed_sites().front().kind == FC_HOOK_DIRECT_CALL_SITE);
    }

    SECTION("instruction observers surround one context owner before displaced execution resumes") {
        // Seed ambient state to test callback counts, paired state, result, and transparent error restoration.
        std::array<PatchCallbackState, 2> callbacks{
            {{.script = LifecycleScript::InstructionHookObserver}, {.script = LifecycleScript::InstructionHookOwner}}};
        auto runtime = runtime_with({patch("AObserver", callbacks[0]), patch("ZOwner", callbacks[1])}, callbacks);
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

        CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
        const auto function = reinterpret_cast<std::int32_t(FC_CALL*)() noexcept>(callbacks[0].image_base + 480);
        SetLastError(0x6161);
        CHECK(function() == 7);
        CHECK(callbacks[0].before_calls == 1);
        CHECK(callbacks[0].after_calls == 1);
        CHECK(callbacks[0].state_mismatches == 0);
        CHECK(callbacks[1].hook_calls == 1);
        CHECK(GetLastError() == 0x6161);
        REQUIRE(runtime->hooks.installed_sites().size() == 1);
        CHECK(runtime->hooks.installed_sites().front().kind == FC_HOOK_INSTRUCTION);
    }
}

TEST_CASE("a builder failure for a previously absent site leaves the next participant eligible to create it") {
    std::array<PatchCallbackState, 2> callbacks{
        {{.script = LifecycleScript::HookBrokenBuilder}, {.script = LifecycleScript::HookObserver}}};
    auto runtime = runtime_with({patch("ABroken", callbacks[0]), patch("BObserver", callbacks[1])}, callbacks);
    const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

    CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
    CHECK(state(*runtime, "ABroken") == fc::planning::PatchState::Failed);
    CHECK(state(*runtime, "BObserver") == fc::planning::PatchState::Installed);
    REQUIRE(runtime->hooks.installed_sites().size() == 1);
    CHECK(runtime->hooks.installed_sites().front().observer_count == 1);
    CHECK(callbacks[0].destroy_count == 1);
}

TEST_CASE("exposure while committing a direct call hook decides whether a later participant may recreate the site") {
#if defined(_M_X64)
    constexpr std::uint32_t direct_call_rva = 404;
#else
    constexpr std::uint32_t direct_call_rva = 408;
#endif

    SECTION("a rejected write exposes nothing and releases the absent site") {
        // The writer rejects the creator before changing bytes; its unpublished site can therefore be destroyed and
        // recreated by the next participant in frozen installation order.
        std::array<PatchCallbackState, 2> callbacks{
            {{.script = LifecycleScript::DirectHookObserver}, {.script = LifecycleScript::DirectHookOwner}}};
        auto runtime = runtime_with({patch("ARejected", callbacks[0]), patch("BSuccessor", callbacks[1])}, callbacks);
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
        DirectHookWriterState writer{.site = callbacks[0].image_base + direct_call_rva,
                                     .failure = DirectHookWriteFailure::NoExposure};

        CHECK(
            fc::runtime::install_ready_patches(plan, *runtime, {.memory_writer = {&writer, &fail_direct_hook_write}}) ==
            fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "ARejected") == fc::planning::PatchState::Failed);
        CHECK(state(*runtime, "BSuccessor") == fc::planning::PatchState::Installed);
        CHECK(runtime->retained_failures.empty());
        REQUIRE(runtime->hooks.installed_sites().size() == 1);
        CHECK(runtime->hooks.installed_sites().front().has_owner);
        CHECK(writer.site_calls == 2);
    }

    SECTION("a restored exposed write retains and blocks the unpublishable site") {
        // The writer publishes the creator's direct CALL before reporting failure. Even exact rollback retains its
        // owners and blocks reconstruction because native exposure crossed the process-lifetime boundary.
        std::array<PatchCallbackState, 2> callbacks{
            {{.script = LifecycleScript::DirectHookObserver}, {.script = LifecycleScript::DirectHookOwner}}};
        auto runtime = runtime_with({patch("AExposed", callbacks[0]), patch("BBlocked", callbacks[1])}, callbacks);
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
        DirectHookWriterState writer{.site = callbacks[0].image_base + direct_call_rva,
                                     .failure = DirectHookWriteFailure::ExposedThenRestored};

        CHECK(
            fc::runtime::install_ready_patches(plan, *runtime, {.memory_writer = {&writer, &fail_direct_hook_write}}) ==
            fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "AExposed") == fc::planning::PatchState::Failed);
        CHECK(state(*runtime, "BBlocked") == fc::planning::PatchState::Skipped);
        REQUIRE(runtime->retained_failures.size() == 1);
        CHECK(runtime->retained_failures.front().rollback == fc::patching::RollbackResult::Restored);
        CHECK(runtime->hooks.installed_sites().empty());
        CHECK(runtime->blocked_claims.size() == 1);
        CHECK(callbacks[0].destroy_count == 0);
        CHECK(callbacks[1].prepare_count == 0);
        CHECK(writer.site_calls == 2);
        CHECK(direct_target(*runtime->target.find(FC_IMAGE_GAME), direct_call_rva) == callbacks[0].image_base + 320);
    }
}

TEST_CASE("bounded revalidation protects inputs from the Prepare phase and supplied evidence only") {
    // Each section changes one post-Plan fact to distinguish required rechecks from writes without evidence.
    SECTION("a changed required address fails before the Prepare phase") {
        PatchCallbackState callback{.script = LifecycleScript::RequiredStale};
        auto runtime = runtime_with({patch("RequiredStale", callback)}, std::span{&callback, 1});
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
        *reinterpret_cast<std::byte*>(callback.required_address) = std::byte{0x30};

        CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "RequiredStale") == fc::planning::PatchState::Failed);
        CHECK(callback.prepare_count == 0);
        CHECK(callback.destroy_count == 1);
    }

    SECTION("the Prepare callback invalidating supplied evidence prevents the Commit phase") {
        PatchCallbackState callback{.script = LifecycleScript::EvidenceMutation};
        auto runtime = runtime_with({patch("EvidenceMutation", callback)}, std::span{&callback, 1});
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

        CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "EvidenceMutation") == fc::planning::PatchState::Failed);
        CHECK(callback.prepare_count == 1);
        CHECK(callback.activate_count == 0);
        CHECK(byte_value(*reinterpret_cast<const std::byte*>(callback.image_base + callback.page_size + 144)) == 0x30);
    }

    SECTION("a requirement losing executable code access fails before the Prepare phase") {
        PatchCallbackState callback{.script = LifecycleScript::CodeAccessStale};
        auto runtime = runtime_with({patch("CodeAccessStale", callback)}, std::span{&callback, 1});
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
        DWORD previous{};
        REQUIRE(VirtualProtect(reinterpret_cast<void*>(callback.image_base), callback.page_size, PAGE_READWRITE,
                               &previous));

        CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "CodeAccessStale") == fc::planning::PatchState::Failed);
        CHECK(callback.prepare_count == 0);
    }

    SECTION("the Prepare callback cannot remove executable access from a pending code mutation") {
        PatchCallbackState callback{.script = LifecycleScript::CodeAccessMutation};
        auto runtime = runtime_with({patch("CodeAccessMutation", callback)}, std::span{&callback, 1});
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

        CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "CodeAccessMutation") == fc::planning::PatchState::Failed);
        CHECK(callback.prepare_count == 1);
        CHECK(callback.activate_count == 0);
        CHECK(byte_value(*reinterpret_cast<const std::byte*>(callback.image_base + 64)) == 0x90);
    }

    SECTION("a write submitted without evidence gains no implicit expected preimage") {
        PatchCallbackState callback{.script = LifecycleScript::RawMutation};
        auto runtime = runtime_with({patch("RawMutation", callback)}, std::span{&callback, 1});
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

        CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "RawMutation") == fc::planning::PatchState::Installed);
        CHECK(byte_value(*reinterpret_cast<const std::byte*>(callback.image_base + callback.page_size + 144)) == 0x20);
    }
}

TEST_CASE("localized failures prune required consumers and preserve independent installation") {
    std::array<PatchCallbackState, 3> callbacks{{{.script = LifecycleScript::PrepareFailure}, {}, {}}};
    auto runtime = runtime_with({patch("Provider", callbacks[0]), patch("Consumer", callbacks[1], {"Provider"}),
                                 patch("Independent", callbacks[2])},
                                callbacks);
    const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

    CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
    CHECK(state(*runtime, "Provider") == fc::planning::PatchState::Failed);
    CHECK(state(*runtime, "Consumer") == fc::planning::PatchState::Skipped);
    CHECK(state(*runtime, "Independent") == fc::planning::PatchState::Installed);
    CHECK(callbacks[0].destroy_count == 1);
    CHECK(callbacks[1].destroy_count == 1);
    CHECK(callbacks[2].activate_count == 1);
}

TEST_CASE("a rejected native data resolution poisons the complete Prepare callback invocation") {
    PatchCallbackState callback{.script = LifecycleScript::IgnoredResolveFailure};
    auto runtime = runtime_with({patch("IgnoredResolveFailure", callback)}, std::span{&callback, 1});
    const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

    CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Completed);
    CHECK(state(*runtime, "IgnoredResolveFailure") == fc::planning::PatchState::Failed);
    CHECK(callback.rejected_resolution_cleared_outputs);
    CHECK(callback.activate_count == 0);
    CHECK(callback.destroy_count == 1);
}

TEST_CASE("an applicable fatal failure policy unwinds first and terminalizes unfinished startup work") {
    std::array<PatchCallbackState, 2> callbacks{{{.script = LifecycleScript::PrepareFailure}, {}}};
    auto runtime = runtime_with(
        {patch("AFatal", callbacks[0], {}, FC_FAILURE_FATAL), patch("BIndependent", callbacks[1])}, callbacks);
    const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);

    CHECK(fc::runtime::install_ready_patches(plan, *runtime) == fc::runtime::InstallationResult::Fatal);
    CHECK(state(*runtime, "AFatal") == fc::planning::PatchState::Failed);
    CHECK(state(*runtime, "BIndependent") == fc::planning::PatchState::Skipped);
    CHECK(callbacks[0].destroy_count == 1);
    CHECK(callbacks[1].destroy_count == 1);
    CHECK(callbacks[1].activate_count == 0);
}

TEST_CASE("a restored exposed failure retains its instance, resources, and exclusive blockers") {
    std::array<PatchCallbackState, 2> callbacks{{{.script = LifecycleScript::RetainedFailure}, {}}};
    {
        auto runtime = runtime_with(
            {patch("RetainedProvider", callbacks[0]), patch("RetainedConsumer", callbacks[1], {"RetainedProvider"})},
            callbacks);
        const auto plan = fc::planning::build_installation_plan(runtime->target, runtime->patches);
        const auto first_address = callbacks[0].image_base + callbacks[0].page_size + 32;
        const auto second_address = callbacks[0].image_base + callbacks[0].page_size * 2 + 32;
        const auto original_first = *reinterpret_cast<const std::byte*>(first_address);
        FailSecondWrite failure{.second = second_address};

        // The second write fails after the first was visible; successful rollback still counts as retained exposure.
        CHECK(fc::runtime::install_ready_patches(plan, *runtime, {.memory_writer = {&failure, &fail_second_write}}) ==
              fc::runtime::InstallationResult::Completed);
        CHECK(state(*runtime, "RetainedProvider") == fc::planning::PatchState::Failed);
        CHECK(state(*runtime, "RetainedConsumer") == fc::planning::PatchState::Skipped);
        REQUIRE(runtime->retained_failures.size() == 1);
        CHECK(runtime->retained_failures.front().rollback == fc::patching::RollbackResult::Restored);
        const auto provider = runtime->catalog.find_patch("RetainedProvider");
        REQUIRE(provider.has_value());
        REQUIRE(runtime->patches.record(*provider).reason.has_value());
        CHECK(runtime->patches.record(*provider).reason->phase == fc::planning::PatchPhase::Commit);
        CHECK(runtime->blocked_claims.size() == 3);
        CHECK(std::ranges::all_of(runtime->blocked_claims, [](const auto& claim) {
            return claim.access == fc::planning::ClaimAccess::Write;
        }));
        CHECK(callbacks[0].destroy_count == 0);
        CHECK(byte_value(*reinterpret_cast<const std::byte*>(first_address)) == byte_value(original_first));
        MEMORY_BASIC_INFORMATION allocation{};
        CHECK(VirtualQuery(reinterpret_cast<const void*>(callbacks[0].data_address), &allocation, sizeof(allocation)) ==
              sizeof(allocation));
        CHECK(allocation.State == MEM_COMMIT);
    }
    // Retained ownership is released only with runtime teardown, with allocation lifetime extending through destroy.
    CHECK(callbacks[0].destroy_count == 1);
    CHECK(callbacks[0].allocation_live_during_destroy);
}

TEST_CASE("transaction classification distinguishes no exposure, restored work, and known residual work") {
    std::array<std::byte, 2> storage{std::byte{0x10}, std::byte{0x11}};
    const std::array first_replacement{std::byte{0x20}};
    const std::array second_replacement{std::byte{0x21}};

    SECTION("a preimage failure leaves native state unexposed") {
        fc::patching::PatchTransaction transaction;
        REQUIRE(transaction.add_write(reinterpret_cast<std::uintptr_t>(&storage[0]), first_replacement, 0, "First"));
        REQUIRE(transaction.add_write(1, second_replacement, 1, "Invalid second"));
        auto result = transaction.commit();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().rollback == fc::patching::RollbackResult::NoExposure);
        CHECK(byte_value(storage[0]) == 0x10);
    }

    SECTION("an injected rollback failure leaves a fully enumerated residual") {
        // The fake publishes the first byte, fails the second, then fails restoration of the known first range.
        FakeWriterState writer{reinterpret_cast<std::uintptr_t>(&storage[0]),
                               reinterpret_cast<std::uintptr_t>(&storage[1])};
        fc::patching::PatchTransaction transaction{{&writer, &fake_residual_write}};
        REQUIRE(transaction.add_write(writer.first, first_replacement, 0, "First"));
        REQUIRE(transaction.add_write(writer.second, second_replacement, 1, "Second"));
        auto result = transaction.commit();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().rollback == fc::patching::RollbackResult::Residual);
        CHECK(result.error().contained);
        CHECK(byte_value(storage[0]) == 0x20);
    }

    SECTION("a later vendor effect failure closes earlier effects in reverse order") {
        // Event signs distinguish publication from restoration and prove the failed second effect is not restored
        // when its adapter reports that it changed nothing.
        std::vector<int> events;
        NativeEffectState first{.events = &events, .identity = 1};
        NativeEffectState second{.events = &events, .identity = 2, .fail_apply = true};
        fc::patching::PatchTransaction transaction;
        REQUIRE(transaction.add_effect({&first, &apply_native_effect, &restore_native_effect}, "First effect"));
        REQUIRE(transaction.add_effect({&second, &apply_native_effect, &restore_native_effect}, "Second effect"));

        auto result = transaction.commit();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().rollback == fc::patching::RollbackResult::Restored);
        CHECK(result.error().contained);
        CHECK(events == std::vector<int>{1, 2, -1});
    }
}
