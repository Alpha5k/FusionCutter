#include "../../fixtures/plugins/shared_hook_probe.hpp"

#include "native_allocation.hpp"
#include "native_memory.hpp"

#include <FusionCutter/SDK.hpp>

#include <catch2/catch_test_macros.hpp>

#include <safetyhook/inline_hook.hpp>

#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

using GetBuilderFn = void(FC_CALL*)(FC_HookBuilder* output) noexcept;
using InvokeMixedFn = std::int32_t(FC_CALL*)(std::uintptr_t entry, std::int32_t first, std::int32_t second,
                                             std::int32_t third, std::int32_t fourth, std::int32_t fifth) noexcept;
using VoidCall = void(FC_CALL*)(std::int32_t value) noexcept;

// Keeps each separately compiled adapter loaded while its builder and participant thunks remain callable.
class ProbeModule final {
  public:
    explicit ProbeModule(const wchar_t* path) : module_(LoadLibraryW(path)) {}
    ProbeModule(const ProbeModule&) = delete;
    ProbeModule& operator=(const ProbeModule&) = delete;
    ~ProbeModule() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return module_ != nullptr;
    }

    template <class Function> [[nodiscard]] Function find(const char* name) const noexcept {
        return reinterpret_cast<Function>(GetProcAddress(module_, name));
    }

  private:
    HMODULE module_{};
};

// Framework-owned prototype storage follows the production W-to-X transition and contains one atomic snapshot cell.
class PrototypeEntry final {
  public:
    explicit PrototypeEntry(const FC_HookBuilder& builder,
                            std::optional<fc::patching::NearConstraint> constraint = std::nullopt)
        : size_(builder.entry_size) {
        auto allocation = fc::patching::NativeAllocation::create(size_, alignof(std::uintptr_t), constraint);
        if (!allocation) {
            return;
        }
        // Give the separately compiled builder one writable framework-owned extent and its stable snapshot slot.
        allocation_ = std::move(*allocation);
        entry_ = reinterpret_cast<std::uint8_t*>(allocation_.address());
        const FC_HookBuildInput input{.struct_size = sizeof(FC_HookBuildInput),
                                      .entry = entry_,
                                      .entry_size = size_,
                                      .snapshot_slot = &snapshot_};
        // A usable prototype follows production's one-way writable-to-executable transition before any call.
        if (builder.build(&input, nullptr) != FC_CALL_OK || !allocation_.make_executable()) {
            allocation_ = {};
            entry_ = nullptr;
        }
    }

    PrototypeEntry(const PrototypeEntry&) = delete;
    PrototypeEntry& operator=(const PrototypeEntry&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return entry_ != nullptr;
    }

    [[nodiscard]] FcHookProbeCall call() const noexcept {
        return reinterpret_cast<FcHookProbeCall>(entry_);
    }

    template <class Call> [[nodiscard]] Call call_as() const noexcept {
        return reinterpret_cast<Call>(entry_);
    }

    void publish(const FC_HookSnapshot& snapshot) noexcept {
        std::atomic_ref{snapshot_}.store(reinterpret_cast<std::uintptr_t>(&snapshot), std::memory_order_release);
    }

  private:
    alignas(std::uintptr_t) std::uintptr_t snapshot_{};
    fc::patching::NativeAllocation allocation_;
    std::uint8_t* entry_{};
    std::uint32_t size_{};
};

// Restores the one test-owned direct CALL even when a later assertion aborts the current test section.
class DirectCallPatch final {
  public:
    DirectCallPatch(std::uintptr_t site, std::uintptr_t destination) : site_(site) {
        // Preserve the exact original instruction before deriving and publishing a replacement rel32 CALL.
        auto original = fc::patching::read_native_memory(site, original_.size());
        if (!original) {
            return;
        }
        std::copy(original->begin(), original->end(), original_.begin());
        const auto following = site + original_.size();
        const auto displacement = static_cast<std::int64_t>(destination) - static_cast<std::int64_t>(following);
        if (displacement < std::numeric_limits<std::int32_t>::min() ||
            displacement > std::numeric_limits<std::int32_t>::max()) {
            return;
        }
        std::array<std::byte, 5> replacement{std::byte{0xe8}};
        const auto encoded = static_cast<std::int32_t>(displacement);
        std::memcpy(replacement.data() + 1, &encoded, sizeof(encoded));
        auto written = fc::patching::system_memory_writer().write(nullptr, site, replacement);
        installed_ = written.has_value();
    }

    DirectCallPatch(const DirectCallPatch&) = delete;
    DirectCallPatch& operator=(const DirectCallPatch&) = delete;
    ~DirectCallPatch() {
        if (installed_) {
            (void)fc::patching::system_memory_writer().write(nullptr, site_, original_);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return installed_;
    }

  private:
    std::uintptr_t site_{};
    std::array<std::byte, 5> original_{};
    bool installed_{};
};

// Supplies a small native caller with one known E8 site so direct call behavior is independent of linker thunks.
class PrototypeDirectCaller final {
  public:
    explicit PrototypeDirectCaller(std::uintptr_t target) {
        // Near placement guarantees that the synthetic E8 operand can name the DLL target on x64 as well as x86.
        constexpr auto rel32_reach = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) - 4096;
        auto allocation = fc::patching::NativeAllocation::create(
            32, alignof(std::uintptr_t),
            fc::patching::NearConstraint{.reference = target, .maximum_distance = rel32_reach});
        if (!allocation) {
            return;
        }
        allocation_ = std::move(*allocation);
        std::array<std::byte, 32> code{};
        code.fill(std::byte{0xcc});
#if defined(_M_X64)
        // Preserve Windows x64 shadow space and alignment around the target call, then distinguish the caller result.
        constexpr std::array prefix{std::byte{0x48}, std::byte{0x83}, std::byte{0xec}, std::byte{0x28}};
        constexpr std::array suffix{std::byte{0x48}, std::byte{0x83}, std::byte{0xc4}, std::byte{0x28},
                                    std::byte{0x83}, std::byte{0xc0}, std::byte{0x01}, std::byte{0xc3}};
        std::copy(prefix.begin(), prefix.end(), code.begin());
        call_offset_ = prefix.size();
        std::copy(suffix.begin(), suffix.end(), code.begin() + call_offset_ + 5);
#else
        // Push right then left from the caller frame, clean the cdecl target arguments, and return to our own caller.
        constexpr std::array prefix{std::byte{0xff}, std::byte{0x74}, std::byte{0x24}, std::byte{0x08},
                                    std::byte{0xff}, std::byte{0x74}, std::byte{0x24}, std::byte{0x08}};
        constexpr std::array suffix{std::byte{0x83}, std::byte{0xc4}, std::byte{0x08}, std::byte{0x83},
                                    std::byte{0xc0}, std::byte{0x01}, std::byte{0xc3}};
        std::copy(prefix.begin(), prefix.end(), code.begin());
        call_offset_ = prefix.size();
        std::copy(suffix.begin(), suffix.end(), code.begin() + call_offset_ + 5);
#endif
        code[call_offset_] = std::byte{0xe8};
        const auto following = allocation_.address() + call_offset_ + 5;
        const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(following);
        if (displacement < std::numeric_limits<std::int32_t>::min() ||
            displacement > std::numeric_limits<std::int32_t>::max()) {
            allocation_ = {};
            return;
        }
        const auto encoded = static_cast<std::int32_t>(displacement);
        std::memcpy(code.data() + call_offset_ + 1, &encoded, sizeof(encoded));
        // Seal only the fully encoded caller so the test never executes partially initialized storage.
        if (!allocation_.initialize(code) || !allocation_.make_executable()) {
            allocation_ = {};
        }
    }

    PrototypeDirectCaller(const PrototypeDirectCaller&) = delete;
    PrototypeDirectCaller& operator=(const PrototypeDirectCaller&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return allocation_.address() != 0;
    }

    [[nodiscard]] FcHookProbeCall call() const noexcept {
        return reinterpret_cast<FcHookProbeCall>(allocation_.address());
    }

    [[nodiscard]] std::uintptr_t call_site() const noexcept {
        return allocation_.address() + call_offset_;
    }

  private:
    fc::patching::NativeAllocation allocation_;
    std::size_t call_offset_{};
};

// Drives the owner DLL's SDK registration, Create callback, and Plan callback while retaining its hook request.
class SdkOwnerRequest final {
  public:
    explicit SdkOwnerRequest(const ProbeModule& module) {
        // Register through the exported ABI to obtain callbacks backed by the owner DLL's private SDK instantiation.
        const auto query =
            module.find<const FC_PluginApi*(FC_CALL*)(std::uint32_t) noexcept>("FusionCutter_QueryPlugin");
        if (query == nullptr) {
            return;
        }
        const auto* api = query(FC_PLUGIN_ABI_GENERATION);
        if (api == nullptr) {
            return;
        }

        DefinitionCapture definition{};
        const FC_RegistrySink registry{
            .struct_size = sizeof(FC_RegistrySink), .context = &definition, .submit = &capture_definition};
        host_.struct_size = sizeof(FC_HostApi);
        if (api->register_plugin(&host_, &registry, nullptr) != FC_CALL_OK || !definition.captured) {
            return;
        }
        callbacks_ = definition.callbacks;

        // Create the real handler under the one target profile declared by the fixture plugin.
        const FC_TargetInfo target{.layout = FC_LAYOUT_GAMESPY_RETAIL,
                                   .role = FC_HOST_ROLE_CLIENT,
#if defined(_M_IX86)
                                   .architecture = FC_ARCH_X86,
#else
                                   .architecture = FC_ARCH_X64,
#endif
                                   .image_profile = {}};
        const FC_CreateContext create{.struct_size = sizeof(FC_CreateContext), .target = target};
        const FC_SettingsView settings{.struct_size = sizeof(FC_SettingsView)};
        if (callbacks_.create(callbacks_.context, &create, &settings, nullptr, &handle_) != FC_CALL_OK ||
            handle_ == nullptr) {
            return;
        }

        // Copy both borrowed hook requests synchronously, mirroring framework ownership after the Plan callback.
        PlanCapture plan_capture{};
        const FC_PlanSink sink{.struct_size = sizeof(FC_PlanSink), .context = &plan_capture, .hook = &capture_hook};
        const FC_PlanContext plan{.struct_size = sizeof(FC_PlanContext), .target = target};
        if (callbacks_.plan(callbacks_.context, handle_, &plan, &sink, nullptr) != FC_CALL_OK ||
            plan_capture.count != plan_capture.requests.size()) {
            return;
        }
        requests_ = plan_capture.requests;
        valid_ = true;
    }

    SdkOwnerRequest(const SdkOwnerRequest&) = delete;
    SdkOwnerRequest& operator=(const SdkOwnerRequest&) = delete;
    ~SdkOwnerRequest() {
        if (handle_ != nullptr) {
            callbacks_.destroy(callbacks_.context, handle_);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid_;
    }

    [[nodiscard]] const FC_HookRequest& request() const noexcept {
        return requests_.front();
    }

    [[nodiscard]] const FC_HookRequest& mixed_request() const noexcept {
        return requests_.back();
    }

  private:
    // Separate captures retain the lifecycle table and the two hook requests whose borrowed inputs expire afterward.
    struct DefinitionCapture {
        FC_PatchCallbacks callbacks{};
        bool captured{};
    };

    struct PlanCapture {
        std::array<FC_HookRequest, 2> requests{};
        std::size_t count{};
    };

    // Accepts only the fixture's single support and extracts the callbacks needed to drive its real lifecycle.
    static FC_SubmitResult FC_CALL capture_definition(void* context, const FC_PluginDefinition* plugin) noexcept {
        auto& capture = *static_cast<DefinitionCapture*>(context);
        if (capture.captured || plugin == nullptr || plugin->patch_count != 1 || plugin->patches == nullptr ||
            plugin->patches[0].support_count != 1 || plugin->patches[0].supports == nullptr) {
            return FC_SUBMIT_REJECTED;
        }
        capture.callbacks = plugin->patches[0].supports[0].callbacks;
        capture.captured = true;
        return FC_SUBMIT_ACCEPTED;
    }

    // Copies each hook request before the NativeCall storage local to the SDK callback expires.
    static FC_SubmitResult FC_CALL capture_hook(void* context, const FC_HookRequest* request) noexcept {
        auto& capture = *static_cast<PlanCapture*>(context);
        if (capture.count == capture.requests.size() || request == nullptr) {
            return FC_SUBMIT_REJECTED;
        }
        capture.requests[capture.count++] = *request;
        return FC_SUBMIT_ACCEPTED;
    }

    // The SDK adapter retains the host table pointer for the handler lifetime, so the fixture owns the table too.
    FC_HostApi host_{};
    FC_PatchCallbacks callbacks_{};
    FC_PatchHandle handle_{};
    std::array<FC_HookRequest, 2> requests_{};
    bool valid_{};
};

// Physical originals set recognizable ambient errors so dispatcher isolation is observable at the outer caller.
std::int32_t FC_CALL original_sum(std::int32_t left, std::int32_t right) noexcept {
    SetLastError(0x7001);
    WSASetLastError(0x7002);
    return left + right;
}

std::atomic<std::int32_t> void_total{};

void FC_CALL original_void(std::int32_t value) noexcept {
    void_total.fetch_add(value, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("separately compiled builders for shared hooks dispatch immutable cross-DLL snapshots") {
    const ProbeModule owner{L"" FC_SHARED_HOOK_OWNER_PATH};
    const ProbeModule observer{L"" FC_SHARED_HOOK_OBSERVER_PATH};
    REQUIRE(owner);
    REQUIRE(observer);

    const auto get_owner_builder = owner.find<GetBuilderFn>("FcHookProbeGetOwnerBuilder");
    const auto get_mixed_builder = owner.find<GetBuilderFn>("FcHookProbeGetMixedBuilder");
    const auto get_void_builder = owner.find<GetBuilderFn>("FcHookProbeGetVoidBuilder");
    const auto get_record_builder = owner.find<GetBuilderFn>("FcHookProbeGetRecordBuilder");
    const auto get_record_argument_builder = owner.find<GetBuilderFn>("FcHookProbeGetRecordArgumentBuilder");
    const auto get_float_builder = owner.find<GetBuilderFn>("FcHookProbeGetFloatBuilder");
    const auto invoke_mixed = owner.find<InvokeMixedFn>("FcHookProbeInvokeMixed");
    const auto invoke_retained_mixed =
        owner.find<std::int32_t(FC_CALL*)(std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                                          std::int32_t) noexcept>("FcHookProbeInvokeRetainedMixedOriginal");
    const auto mixed_owner = owner.find<std::uintptr_t>("FcHookProbeMixedOwnerThunk");
    const auto record_owner = owner.find<std::uintptr_t>("FcHookProbeRecordOwnerThunk");
    const auto invoke_record =
        owner.find<FcHookProbeRecord(FC_CALL*)(std::uintptr_t, std::int32_t, std::int32_t) noexcept>(
            "FcHookProbeInvokeRecord");
    const auto record_argument_owner = owner.find<std::uintptr_t>("FcHookProbeRecordArgumentOwnerThunk");
    const auto invoke_record_argument =
        owner.find<std::int32_t(FC_CALL*)(std::uintptr_t, FcHookProbeRecord, std::int32_t) noexcept>(
            "FcHookProbeInvokeRecordArgument");
    const auto float_owner = owner.find<std::uintptr_t>("FcHookProbeFloatOwnerThunk");
    const auto invoke_float =
        owner.find<double(FC_CALL*)(std::uintptr_t, double, float) noexcept>("FcHookProbeInvokeFloat");
    const auto direct_target = owner.find<FcHookProbeCall>("FcHookProbeDirectTarget");
    const auto get_observer_builder = observer.find<GetBuilderFn>("FcHookProbeGetObserverBuilder");
    const auto owner_thunk = owner.find<FcHookProbeCall>("FcHookProbeOwnerThunk");
    const auto before = observer.find<std::uintptr_t>("FcHookProbeBeforeThunk");
    const auto after = observer.find<std::uintptr_t>("FcHookProbeAfterThunk");
    REQUIRE(get_owner_builder != nullptr);
    REQUIRE(get_mixed_builder != nullptr);
    REQUIRE(get_void_builder != nullptr);
    REQUIRE(get_record_builder != nullptr);
    REQUIRE(get_record_argument_builder != nullptr);
    REQUIRE(get_float_builder != nullptr);
    REQUIRE(invoke_mixed != nullptr);
    REQUIRE(invoke_retained_mixed != nullptr);
    REQUIRE(mixed_owner != 0);
    REQUIRE(record_owner != 0);
    REQUIRE(invoke_record != nullptr);
    REQUIRE(record_argument_owner != 0);
    REQUIRE(invoke_record_argument != nullptr);
    REQUIRE(float_owner != 0);
    REQUIRE(invoke_float != nullptr);
    REQUIRE(direct_target != nullptr);
    REQUIRE(get_observer_builder != nullptr);
    REQUIRE(owner_thunk != nullptr);
    REQUIRE(before != 0);
    REQUIRE(after != 0);

    SdkOwnerRequest sdk_owner{owner};
    REQUIRE(sdk_owner);
    REQUIRE(sdk_owner.request().builder.build != nullptr);
    REQUIRE(sdk_owner.request().callback != 0);
    REQUIRE(sdk_owner.request().bind_original != nullptr);

    FC_HookBuilder owner_builder{};
    FC_HookBuilder mixed_builder{};
    FC_HookBuilder void_builder{};
    FC_HookBuilder record_builder{};
    FC_HookBuilder record_argument_builder{};
    FC_HookBuilder float_builder{};
    FC_HookBuilder observer_builder{};
    get_owner_builder(&owner_builder);
    get_mixed_builder(&mixed_builder);
    get_void_builder(&void_builder);
    get_record_builder(&record_builder);
    get_record_argument_builder(&record_argument_builder);
    get_float_builder(&float_builder);
    get_observer_builder(&observer_builder);
    REQUIRE(owner_builder.entry_size == fc::detail::kGeneratedHookEntrySize);
    REQUIRE(observer_builder.entry_size == owner_builder.entry_size);

    // A disabled entry hook initially invokes only Original, then admits observer and owner snapshots.
    {
        PrototypeEntry function_entry{sdk_owner.request().builder};
        REQUIRE(function_entry);
        auto prepared_hook =
            safetyhook::InlineHook::create(direct_target, function_entry.call(), safetyhook::InlineHook::StartDisabled);
        REQUIRE(prepared_hook.has_value());
        auto physical_hook = std::move(*prepared_hook);
        const auto trampoline = physical_hook.original<FcHookProbeCall>();
        REQUIRE(trampoline != nullptr);

        FcHookProbeObserverContext physical_observer{};
        const FC_HookObserverEntry physical_observer_entry{
            .context = &physical_observer, .before = before, .after = after, .state_offset = 0, .state_size = 4};
        const FC_HookSnapshot closed{.struct_size = sizeof(FC_HookSnapshot),
                                     .original = reinterpret_cast<std::uintptr_t>(trampoline)};
        function_entry.publish(closed);
        REQUIRE(physical_hook.enable().has_value());
        CHECK(direct_target(3, 4) == 7);

        const FC_HookSnapshot observed{.struct_size = sizeof(FC_HookSnapshot),
                                       .original = reinterpret_cast<std::uintptr_t>(trampoline),
                                       .observers = &physical_observer_entry,
                                       .observer_count = 1,
                                       .total_state_size = 4};
        function_entry.publish(observed);
        CHECK(direct_target(3, 4) == 7);
        CHECK(physical_observer.before_calls == 1);

        // Binding the trampoline proves the public Original handle survives the SDK Plan callback across the DLL.
        sdk_owner.request().bind_original(sdk_owner.request().original_context,
                                          reinterpret_cast<std::uintptr_t>(trampoline));
        const FC_HookSnapshot owned{
            .struct_size = sizeof(FC_HookSnapshot),
            .original = reinterpret_cast<std::uintptr_t>(trampoline),
            .owner = {.context = sdk_owner.request().context, .callback = sdk_owner.request().callback},
            .observers = &physical_observer_entry,
            .observer_count = 1,
            .total_state_size = 4};
        function_entry.publish(owned);
        CHECK(direct_target(3, 4) == 20);
        CHECK(physical_observer.before_calls == 2);
        CHECK(physical_observer.after_calls == 2);
        CHECK(GetLastError() == 0x7102);
        CHECK(WSAGetLastError() == 0x7102);
        REQUIRE(physical_hook.disable().has_value());
    }

    // A typed direct call site reaches the same dispatcher by CALL and leaves the global target untouched.
    {
        PrototypeDirectCaller direct_caller{reinterpret_cast<std::uintptr_t>(direct_target)};
        REQUIRE(direct_caller);
        const auto call_site = direct_caller.call_site();
        constexpr auto rel32_reach = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) - 4096;
        PrototypeEntry call_entry{
            sdk_owner.request().builder,
            fc::patching::NearConstraint{.reference = call_site, .maximum_distance = rel32_reach}};
        REQUIRE(call_entry);
        sdk_owner.request().bind_original(sdk_owner.request().original_context,
                                          reinterpret_cast<std::uintptr_t>(direct_target));
        const FC_HookSnapshot call_snapshot{
            .struct_size = sizeof(FC_HookSnapshot),
            .original = reinterpret_cast<std::uintptr_t>(direct_target),
            .owner = {.context = sdk_owner.request().context, .callback = sdk_owner.request().callback}};
        call_entry.publish(call_snapshot);
        DirectCallPatch patch{call_site, reinterpret_cast<std::uintptr_t>(call_entry.call())};
        REQUIRE(patch);
        CHECK(direct_caller.call()(4, 5) == 23);
        CHECK(direct_target(4, 5) == 9);
    }

    // Two entries built by different DLLs but with the same Call prove that the slot determines physical site identity.
    PrototypeEntry first{owner_builder};
    PrototypeEntry second{observer_builder};
    REQUIRE(first);
    REQUIRE(second);

    FcHookProbeObserverContext first_observer{};
    FcHookProbeObserverContext second_observer{};
    const FC_HookObserverEntry first_observer_entry{
        .context = &first_observer, .before = before, .after = after, .state_offset = 0, .state_size = 4};
    const FC_HookObserverEntry second_observer_entry{
        .context = &second_observer, .before = before, .after = after, .state_offset = 0, .state_size = 4};
    const FC_HookSnapshot first_original{.struct_size = sizeof(FC_HookSnapshot),
                                         .original = reinterpret_cast<std::uintptr_t>(&original_sum),
                                         .observers = &first_observer_entry,
                                         .observer_count = 1,
                                         .total_state_size = 4};
    const FC_HookSnapshot second_original{.struct_size = sizeof(FC_HookSnapshot),
                                          .original = reinterpret_cast<std::uintptr_t>(&original_sum),
                                          .observers = &second_observer_entry,
                                          .observer_count = 1,
                                          .total_state_size = 4};
    first.publish(first_original);
    second.publish(second_original);

    CHECK(first.call()(2, 3) == 5);
    CHECK(second.call()(7, 11) == 18);
    CHECK(first_observer.before_calls == 1);
    CHECK(second_observer.before_calls == 1);

    // Snapshot replacement adds the modifying owner without changing either entry or its embedded site identity.
    FcHookProbeOwnerContext owner_context{.original = reinterpret_cast<std::uintptr_t>(&original_sum), .adjustment = 9};
    const FC_HookSnapshot first_owned{
        .struct_size = sizeof(FC_HookSnapshot),
        .original = reinterpret_cast<std::uintptr_t>(&original_sum),
        .owner = {.context = &owner_context, .callback = reinterpret_cast<std::uintptr_t>(owner_thunk)},
        .observers = &first_observer_entry,
        .observer_count = 1,
        .total_state_size = 4};
    first.publish(first_owned);
    const auto owned_result = first.call()(5, 6);
    const auto owned_windows_error = GetLastError();
    const auto owned_winsock_error = WSAGetLastError();
    CHECK(owned_result == 20);
    CHECK(second.call()(5, 6) == 11);
    CHECK(owner_context.calls == 1);
    CHECK(owned_windows_error == 0x7002);
    CHECK(owned_winsock_error == 0x7002);

    // A nested call at the same site must not overwrite the outer observer's paired state.
    owner_context.nested_entry = reinterpret_cast<std::uintptr_t>(first.call());
    CHECK(first.call()(5, 6) == 32);
    CHECK(first_observer.state_mismatches == 0);
    owner_context.nested_entry = 0;

    // The same builder supports a NativeCall with mixed register and stack arguments plus an entry returning void.
    PrototypeEntry mixed{mixed_builder};
    PrototypeEntry mixed_original{mixed_builder};
    PrototypeEntry void_entry{void_builder};
    PrototypeEntry record_entry{record_builder};
    PrototypeEntry record_argument_entry{record_argument_builder};
    PrototypeEntry float_entry{float_builder};
    REQUIRE(mixed);
    REQUIRE(mixed_original);
    REQUIRE(void_entry);
    REQUIRE(record_entry);
    REQUIRE(record_argument_entry);
    REQUIRE(float_entry);
    const FC_HookSnapshot mixed_original_snapshot{.struct_size = sizeof(FC_HookSnapshot),
                                                  .owner = {.callback = mixed_owner}};
    const FC_HookSnapshot mixed_snapshot{.struct_size = sizeof(FC_HookSnapshot),
                                         .original = reinterpret_cast<std::uintptr_t>(mixed_original.call())};
    const FC_HookSnapshot void_snapshot{.struct_size = sizeof(FC_HookSnapshot),
                                        .original = reinterpret_cast<std::uintptr_t>(&original_void)};
    const FC_HookSnapshot record_snapshot{.struct_size = sizeof(FC_HookSnapshot), .owner = {.callback = record_owner}};
    const FC_HookSnapshot record_argument_snapshot{.struct_size = sizeof(FC_HookSnapshot),
                                                   .owner = {.callback = record_argument_owner}};
    const FC_HookSnapshot float_snapshot{.struct_size = sizeof(FC_HookSnapshot), .owner = {.callback = float_owner}};
    mixed_original.publish(mixed_original_snapshot);
    mixed.publish(mixed_snapshot);
    sdk_owner.mixed_request().bind_original(sdk_owner.mixed_request().original_context,
                                            reinterpret_cast<std::uintptr_t>(mixed_original.call()));
    void_entry.publish(void_snapshot);
    record_entry.publish(record_snapshot);
    record_argument_entry.publish(record_argument_snapshot);
    float_entry.publish(float_snapshot);
#if defined(_M_IX86)
    CHECK(invoke_mixed(reinterpret_cast<std::uintptr_t>(mixed.call()), 1, 2, 3, 0, 0) == 6);

    // The x86 shorthand matrix proves that cdecl, stdcall, fastcall, and thiscall normalize into this one builder.
    using InvokeConventionFn = std::int32_t(FC_CALL*)(std::uintptr_t, std::int32_t, std::int32_t) noexcept;
    const auto verify_convention = [&](const char* builder_name, const char* owner_name, const char* invoke_name,
                                       std::int32_t expected) {
        const auto get_builder = owner.find<GetBuilderFn>(builder_name);
        const auto convention_owner = owner.find<std::uintptr_t>(owner_name);
        const auto invoke = owner.find<InvokeConventionFn>(invoke_name);
        REQUIRE(get_builder != nullptr);
        REQUIRE(convention_owner != 0);
        REQUIRE(invoke != nullptr);
        FC_HookBuilder builder{};
        get_builder(&builder);
        PrototypeEntry entry{builder};
        REQUIRE(entry);
        const FC_HookSnapshot snapshot{.struct_size = sizeof(FC_HookSnapshot), .owner = {.callback = convention_owner}};
        entry.publish(snapshot);
        CHECK(invoke(reinterpret_cast<std::uintptr_t>(entry.call()), 3, 4) == expected);
        CHECK(invoke(reinterpret_cast<std::uintptr_t>(entry.call()), 3, 4) == expected);
    };
    verify_convention("FcHookProbeGetStdcallBuilder", "FcHookProbeStdcallOwnerThunk", "FcHookProbeInvokeStdcall", 34);
    verify_convention("FcHookProbeGetFastcallBuilder", "FcHookProbeFastcallOwnerThunk", "FcHookProbeInvokeFastcall",
                      34);
    verify_convention("FcHookProbeGetThiscallBuilder", "FcHookProbeThiscallOwnerThunk", "FcHookProbeInvokeThiscall", 7);
#else
    CHECK(invoke_mixed(reinterpret_cast<std::uintptr_t>(mixed_original.call()), 1, 2, 3, 4, 5) == 15);
    CHECK(invoke_mixed(reinterpret_cast<std::uintptr_t>(mixed.call()), 1, 2, 3, 4, 5) == 15);
#endif
#if defined(_M_IX86)
    constexpr std::int32_t retained_mixed_result = 6;
#else
    constexpr std::int32_t retained_mixed_result = 15;
#endif
    CHECK(invoke_retained_mixed(1, 2, 3, 4, 5) == retained_mixed_result);
    sdk_owner.mixed_request().bind_original(sdk_owner.mixed_request().original_context, 0);
    const auto record = invoke_record(reinterpret_cast<std::uintptr_t>(record_entry.call()), 8, 13);
    CHECK(record.left == 8);
    CHECK(record.right == 13);
    CHECK(record.total == 21);
    CHECK(invoke_record_argument(reinterpret_cast<std::uintptr_t>(record_argument_entry.call()), record, 5) == 26);
    CHECK(invoke_float(reinterpret_cast<std::uintptr_t>(float_entry.call()), 1.25, 2.5F) == 3.75);
    void_total.store(0, std::memory_order_relaxed);
    void_entry.call_as<VoidCall>()(7);
    CHECK(void_total.load(std::memory_order_relaxed) == 7);

    // Concurrent calls need separate frames for paired state and must prevent observers from corrupting ambient errors.
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t calls_per_thread = 200;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&, thread] {
            for (std::size_t call = 0; call < calls_per_thread; ++call) {
                const auto left = static_cast<std::int32_t>(thread);
                const auto right = static_cast<std::int32_t>(call);
                if (first.call()(left, right) != left + right + owner_context.adjustment) {
                    InterlockedIncrement(&first_observer.state_mismatches);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(first_observer.state_mismatches == 0);
    CHECK(first_observer.before_calls == static_cast<LONG>(4 + thread_count * calls_per_thread));
    CHECK(first_observer.after_calls == first_observer.before_calls);
}
