#include "hook_registry.hpp"

#include "../fatal_boundary.hpp"

#include "native_allocation.hpp"
#include "native_memory.hpp"

#include "../catalog/callback_error.hpp"
#include "../catalog/definition_copy.hpp"
#include "../planning/evidence_validation.hpp"
#include "../planning/native_address.hpp"

#include <safetyhook/inline_hook.hpp>
#include <safetyhook/mid_hook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fc::patching {
namespace {

#if defined(_M_IX86)
static_assert(sizeof(safetyhook::Context) == sizeof(FC_CpuContext));
static_assert(offsetof(safetyhook::Context, trampoline_esp) == offsetof(FC_CpuContext, resume_esp));
static_assert(offsetof(safetyhook::Context, eip) == offsetof(FC_CpuContext, eip));
#else
static_assert(sizeof(safetyhook::Context) == sizeof(FC_CpuContext));
static_assert(offsetof(safetyhook::Context, trampoline_rsp) == offsetof(FC_CpuContext, resume_rsp));
static_assert(offsetof(safetyhook::Context, rip) == offsetof(FC_CpuContext, rip));
#endif

constexpr std::size_t kEntryGuardSize = 32;

// These small adapters give every registry failure and copied contribution a stable identity for patch diagnostics.
[[nodiscard]] planning::FailureReason hook_failure(std::string message, std::string operation) {
    return {.message = std::move(message), .phase = planning::PatchPhase::Prepare, .operation = std::move(operation)};
}

[[nodiscard]] const catalog::SupportDefinitionRecord& selected_support(const catalog::Catalog& catalog,
                                                                       catalog::PatchIndex patch) noexcept {
    const auto& definition = catalog.patch(patch);
    return definition.supports[*definition.selected_support];
}

[[nodiscard]] std::string operation_name(const planning::HookOperation& hook) {
    if (!hook.location.name.empty()) {
        return hook.location.name;
    }
    if (!hook.location.label.empty()) {
        return hook.location.label;
    }
    return "Prepare shared hook";
}

// A retained participant is independent of the submitted patch plan cleared after installation.
struct ParticipantRecord {
    catalog::PatchIndex patch;
    bool observer{};
    void* context{};
    std::uintptr_t callback{};
    std::uintptr_t after{};
    void* original_context{};
    FC_BindOriginalFn bind_original{};
    std::uint32_t state_size{};
    std::uint32_t state_alignment{};
};

// Keeps the observer array at a permanent address for as long as its immutable snapshot can be dispatched.
struct SnapshotStorage {
    std::vector<FC_HookObserverEntry> observers;
    FC_HookSnapshot snapshot{};
};

using PhysicalHook = std::variant<std::monostate, safetyhook::InlineHook, safetyhook::MidHook>;

// Heap allocation makes the embedded slot address permanent before a plugin builder encodes it into native code.
struct HookSite {
    FC_TargetImage image{};
    std::uint32_t rva{};
    FC_HookKind kind{};
    std::uint32_t overwrite_size{};
    std::uintptr_t native_address{};
    std::uintptr_t entry_address{};
    std::uintptr_t original{};
    alignas(std::uintptr_t) std::uintptr_t snapshot_slot{};
    NativeAllocation entry_allocation;
    std::vector<std::byte> original_bytes;
    FC_HookSnapshot closed_snapshot{};
    std::vector<ParticipantRecord> participants;
    std::vector<std::unique_ptr<SnapshotStorage>> snapshots;
    std::size_t projection_index{std::numeric_limits<std::size_t>::max()};
    planning::InstalledHookSite prepared_projection;
    // Declared last so teardown closes a vendor hook before snapshots or executable entry storage disappear.
    PhysicalHook physical;
};

[[nodiscard]] bool same_site(const HookSite& site, FC_TargetImage image, std::uint32_t rva) noexcept {
    return site.image == image && site.rva == rva;
}

[[nodiscard]] ParticipantRecord participant(catalog::PatchIndex patch,
                                            const planning::HookOperation& operation) noexcept {
    return {.patch = patch,
            .observer = operation.observer,
            .context = operation.context,
            .callback = operation.callback,
            .after = operation.after,
            .original_context = operation.original_context,
            .bind_original = operation.bind_original,
            .state_size = operation.state_size,
            .state_alignment = operation.state_alignment};
}

// Function entry Original handles use framework trampolines; direct call handles point to game code.
[[nodiscard]] HookResourceView resource_view(const HookSite& site) noexcept {
    return {.entry_address = site.entry_allocation.address(),
            .entry_size = site.entry_allocation.size(),
            .trampoline_address = site.kind == FC_HOOK_FUNCTION_ENTRY ? site.original : 0};
}

[[nodiscard]] std::uint32_t align_state(std::uint32_t offset, std::uint32_t alignment) noexcept {
    return alignment == 0 ? offset : static_cast<std::uint32_t>((offset + alignment - 1) & ~(alignment - 1));
}

// Rebuilds the entire immutable view because installed snapshots are never mutated or reclaimed.
[[nodiscard]] std::unique_ptr<SnapshotStorage> make_snapshot(const catalog::Catalog& catalog, const HookSite& site,
                                                             const ParticipantRecord& addition) {
    // Sort the complete participant set again so late installation timing cannot change callback order.
    std::vector<ParticipantRecord> participants = site.participants;
    participants.push_back(addition);
    std::ranges::sort(participants, [&](const ParticipantRecord& left, const ParticipantRecord& right) {
        const auto left_id = catalog::fold_ascii(catalog.patch(left.patch).id);
        const auto right_id = catalog::fold_ascii(catalog.patch(right.patch).id);
        return left_id != right_id ? left_id < right_id : left.patch.value < right.patch.value;
    });

    // Assign one owner and tightly packed, alignment-correct observer state slices in that stable order.
    auto storage = std::make_unique<SnapshotStorage>();
    storage->observers.reserve(participants.size());
    std::uint32_t state_size{};
    for (const auto& current : participants) {
        if (!current.observer) {
            storage->snapshot.owner = {.context = current.context, .callback = current.callback};
            continue;
        }
        state_size = align_state(state_size, current.state_alignment);
        storage->observers.push_back({.context = current.context,
                                      .before = current.callback,
                                      .after = current.after,
                                      .state_offset = state_size,
                                      .state_size = current.state_size});
        state_size += current.state_size;
    }
    // Point the published view only at storage retained by this process-lifetime snapshot owner.
    storage->snapshot.struct_size = sizeof(FC_HookSnapshot);
    storage->snapshot.original = site.original;
    storage->snapshot.observers = storage->observers.empty() ? nullptr : storage->observers.data();
    storage->snapshot.observer_count = static_cast<std::uint32_t>(storage->observers.size());
    storage->snapshot.total_state_size = state_size;
    return storage;
}

// Lets a plugin-owned builder fill only its declared entry while guards detect writes across the ABI boundary.
[[nodiscard]] std::expected<void, planning::FailureReason>
build_entry(HookSite& site, const planning::HookOperation& operation, std::optional<NearConstraint> constraint) {
    const auto entry_size = static_cast<std::size_t>(operation.builder.entry_size);
    const auto allocation_size = entry_size + kEntryGuardSize * 2;
    auto allocation = NativeAllocation::create(allocation_size, 16, constraint);
    if (!allocation) {
        return std::unexpected(hook_failure(allocation.error(), operation_name(operation)));
    }
    std::vector<std::byte> guarded(allocation_size, std::byte{0xa5});
    if (auto initialized = allocation->initialize(guarded); !initialized) {
        return std::unexpected(hook_failure(initialized.error(), operation_name(operation)));
    }

    // The builder receives the writable interior only; the registry retains and later seals the complete allocation.
    site.entry_allocation = std::move(*allocation);
    site.entry_address = site.entry_allocation.address() + kEntryGuardSize;
    auto* entry = reinterpret_cast<std::uint8_t*>(site.entry_address);
    const FC_HookBuildInput input{.struct_size = sizeof(FC_HookBuildInput),
                                  .entry = entry,
                                  .entry_size = operation.builder.entry_size,
                                  .snapshot_slot = &site.snapshot_slot};
    catalog::CallbackError callback_error;
    const auto error = callback_error.sink();
    FC_CallStatus status = FC_CALL_FAILED;
    try {
        status = operation.builder.build(&input, &error);
    } catch (...) {
        return std::unexpected(
            hook_failure("Hook builder threw across its nonthrowing ABI", operation_name(operation)));
    }
    // A successful callback is accepted only when both sentinels prove it respected the advertised extent.
    const auto* allocation_bytes = reinterpret_cast<const std::byte*>(site.entry_allocation.address());
    const auto leading_guard = std::span{allocation_bytes, kEntryGuardSize};
    const auto trailing_guard = std::span{allocation_bytes + kEntryGuardSize + entry_size, kEntryGuardSize};
    const auto guard_intact = [](std::span<const std::byte> guard) {
        return std::ranges::all_of(guard, [](std::byte value) {
            return value == std::byte{0xa5};
        });
    };
    if (status != FC_CALL_OK || !guard_intact(leading_guard) || !guard_intact(trailing_guard)) {
        const auto message = callback_error.supplied && !callback_error.message.empty() ? callback_error.message
                             : status == FC_CALL_OK ? "Hook builder wrote outside its declared entry extent"
                                                    : "Hook builder failed";
        return std::unexpected(hook_failure(message, operation_name(operation)));
    }
    if (auto executable = site.entry_allocation.make_executable(); !executable) {
        return std::unexpected(hook_failure(executable.error(), operation_name(operation)));
    }
    return {};
}

[[nodiscard]] std::vector<std::byte> copy_original_bytes(std::span<const std::uint8_t> bytes) {
    std::vector<std::byte> result(bytes.size());
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

// Prepares a physical site without exposing bytes; the Commit phase owns its enable or direct call write.
[[nodiscard]] std::expected<std::unique_ptr<HookSite>, planning::FailureReason>
prepare_absent_site(const targets::RecognizedTarget& target, FC_TargetImage image_id,
                    const planning::OperationRecord& record, PatchTransaction& transaction) {
    const auto& operation = std::get<planning::HookOperation>(record.payload);
    const auto* image = target.find(image_id);
    if (image == nullptr || operation.location.rva >= image->info().size ||
        image->info().base > std::numeric_limits<std::uintptr_t>::max() - operation.location.rva) {
        return std::unexpected(hook_failure("Shared hook image address is unavailable", operation_name(operation)));
    }
    auto site = std::make_unique<HookSite>();
    site->image = image_id;
    site->rva = operation.location.rva;
    site->kind = operation.kind;
    site->overwrite_size = operation.overwrite_size;
    site->native_address = image->info().base + operation.location.rva;

    // Only a direct CALL must encode the entry itself in rel32 reach; vendor hooks can bridge their own destination.
    std::optional<NearConstraint> constraint;
    if (operation.kind == FC_HOOK_DIRECT_CALL_SITE && target.architecture() == FC_ARCH_X64) {
        constexpr auto reach = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) - 4096;
        constraint = NearConstraint{site->native_address, reach};
    }
    if (auto built = build_entry(*site, operation, constraint); !built) {
        return std::unexpected(built.error());
    }

    // Vendor hooks remain disabled through the Prepare phase, while a direct call site contributes a transaction
    // write. Both paths therefore share exposure during the Commit phase and rollback classification.
    if (operation.kind == FC_HOOK_FUNCTION_ENTRY) {
        auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(site->native_address),
                                                   reinterpret_cast<void*>(site->entry_address),
                                                   safetyhook::InlineHook::StartDisabled);
        if (!hook || hook->original_bytes().size() != operation.overwrite_size) {
            return std::unexpected(hook_failure("Function hook could not preserve the validated overwrite shape",
                                                operation_name(operation)));
        }
        site->original = reinterpret_cast<std::uintptr_t>(hook->original<void*>());
        site->original_bytes = copy_original_bytes(hook->original_bytes());
        site->physical = std::move(*hook);
    } else if (operation.kind == FC_HOOK_INSTRUCTION) {
        auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(site->native_address),
                                                reinterpret_cast<safetyhook::MidHookFn>(site->entry_address),
                                                safetyhook::MidHook::StartDisabled);
        if (!hook || hook->original_bytes().size() != operation.overwrite_size) {
            return std::unexpected(hook_failure("Instruction hook could not preserve the validated overwrite shape",
                                                operation_name(operation)));
        }
        site->original_bytes = copy_original_bytes(hook->original_bytes());
        site->physical = std::move(*hook);
    } else {
        auto original =
            planning::validate_direct_branch(*image, target.architecture(), operation.location, FC_REDIRECT_CALL);
        if (!original) {
            return std::unexpected(hook_failure(original.error(), operation_name(operation)));
        }
        site->original = *original;
        auto bytes = read_native_memory(site->native_address, operation.overwrite_size);
        if (!bytes) {
            return std::unexpected(hook_failure(bytes.error(), operation_name(operation)));
        }
        site->original_bytes = std::move(*bytes);
        const auto displacement =
            planning::encode_rel32(site->native_address + 5, site->entry_address, target.architecture());
        if (!displacement) {
            return std::unexpected(
                hook_failure("Direct call dispatcher is outside signed rel32 reach", operation_name(operation)));
        }
        std::array<std::byte, 5> replacement{std::byte{0xe8}};
        std::memcpy(replacement.data() + 1, &*displacement, sizeof(*displacement));
        if (auto added =
                transaction.add_write(site->native_address, replacement, record.index, operation_name(operation));
            !added) {
            return std::unexpected(hook_failure(added.error(), operation_name(operation)));
        }
    }

    // The initial slot remains safe if native publication races after the Commit phase begins.
    site->closed_snapshot = {.struct_size = sizeof(FC_HookSnapshot), .original = site->original};
    std::atomic_ref{site->snapshot_slot}.store(reinterpret_cast<std::uintptr_t>(&site->closed_snapshot),
                                               std::memory_order_release);
    site->prepared_projection = {.image = site->image,
                                 .rva = site->rva,
                                 .kind = site->kind,
                                 .native_call = operation.native_call,
                                 .overwrite_size = site->overwrite_size,
                                 .original_bytes = site->original_bytes};
    return site;
}

// These callbacks let PatchTransaction publish and reverse vendor-owned hooks without assuming their byte layout.
[[nodiscard]] std::expected<void, NativeWriteFailure> enable_physical(void* context) {
    auto& site = *static_cast<HookSite*>(context);
    if (auto* hook = std::get_if<safetyhook::InlineHook>(&site.physical)) {
        auto enabled = hook->enable();
        return enabled ? std::expected<void, NativeWriteFailure>{}
                       : std::unexpected(
                             NativeWriteFailure{"SafetyHook could not enable a function hook", hook->enabled()});
    }
    if (auto* hook = std::get_if<safetyhook::MidHook>(&site.physical)) {
        auto enabled = hook->enable();
        return enabled ? std::expected<void, NativeWriteFailure>{}
                       : std::unexpected(
                             NativeWriteFailure{"SafetyHook could not enable an instruction hook", hook->enabled()});
    }
    return std::unexpected(NativeWriteFailure{"Prepared shared hook has no physical enable action"});
}

[[nodiscard]] std::expected<void, NativeWriteFailure> disable_physical(void* context) {
    auto& site = *static_cast<HookSite*>(context);
    if (auto* hook = std::get_if<safetyhook::InlineHook>(&site.physical)) {
        auto disabled = hook->disable();
        return disabled ? std::expected<void, NativeWriteFailure>{}
                        : std::unexpected(NativeWriteFailure{"SafetyHook could not restore a function hook", true});
    }
    if (auto* hook = std::get_if<safetyhook::MidHook>(&site.physical)) {
        auto disabled = hook->disable();
        return disabled ? std::expected<void, NativeWriteFailure>{}
                        : std::unexpected(NativeWriteFailure{"SafetyHook could not restore an instruction hook", true});
    }
    return std::unexpected(NativeWriteFailure{"Prepared shared hook has no physical restore action", true});
}

} // namespace

struct HookPreparation::Impl {
    // Each item couples the proposed immutable view with any new physical owner and Original binding it depends on.
    struct Item {
        HookSite* site{};
        ParticipantRecord participant;
        std::string operation;
        std::unique_ptr<SnapshotStorage> snapshot;
        // This follows snapshot so reverse destruction closes a retained physical site before its prepared data.
        std::unique_ptr<HookSite> new_site;
        bool original_bound{};
    };

    std::vector<Item> items;

    void clear_original_bindings() noexcept {
        for (auto& item : items) {
            if (!item.original_bound || item.participant.bind_original == nullptr) {
                continue;
            }
            try {
                item.participant.bind_original(item.participant.original_context, 0);
            } catch (...) {
                // Binding cleanup is a best-effort ABI containment path before the owning plugin is destroyed.
            }
            item.original_bound = false;
        }
    }
};

// Separates permanent installed sites from the projections borrowed by validation of later patches.
struct HookRegistry::Impl {
    std::vector<std::unique_ptr<HookSite>> sites;
    std::vector<planning::InstalledHookSite> projections;

    [[nodiscard]] HookSite* find(FC_TargetImage image, std::uint32_t rva) const noexcept {
        const auto found = std::ranges::find_if(sites, [&](const auto& site) {
            return same_site(*site, image, rva);
        });
        return found == sites.end() ? nullptr : found->get();
    }
};

HookPreparation::HookPreparation() noexcept = default;

HookPreparation::HookPreparation(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

HookPreparation::HookPreparation(HookPreparation&&) noexcept = default;
HookPreparation& HookPreparation::operator=(HookPreparation&&) noexcept = default;
HookPreparation::~HookPreparation() = default;

void HookPreparation::clear_original_bindings() noexcept {
    if (implementation_ != nullptr) {
        implementation_->clear_original_bindings();
    }
}

void HookPreparation::visit_resources(void* context, HookResourceVisitor visitor) const noexcept {
    if (implementation_ == nullptr || visitor == nullptr) {
        return;
    }
    // Only absent sites allocate physical state under this attempt; the registry continues to own existing sites.
    for (const auto& item : implementation_->items) {
        if (item.new_site != nullptr) {
            visitor(context, resource_view(*item.new_site));
        }
    }
}

HookRegistry::HookRegistry() : implementation_(std::make_unique<Impl>()) {}
HookRegistry::~HookRegistry() = default;

void HookRegistry::reserve(std::size_t maximum_sites) {
    implementation_->sites.reserve(maximum_sites);
    implementation_->projections.reserve(maximum_sites);
}

std::span<const planning::InstalledHookSite> HookRegistry::installed_sites() const noexcept {
    return implementation_->projections;
}

std::expected<HookPreparation, planning::FailureReason>
HookRegistry::prepare_patch(const targets::RecognizedTarget& target, const catalog::Catalog& catalog,
                            const planning::PatchWorkRecord& patch, PatchTransaction& transaction,
                            std::vector<planning::MemoryClaim>& claims) {
    auto prepared = std::make_unique<HookPreparation::Impl>();
    try {
        // Build every contribution as unpublished attempt-owned state; failure can discard the whole set safely.
        for (const auto& record : patch.plan.operations) {
            const auto* operation = std::get_if<planning::HookOperation>(&record.payload);
            if (operation == nullptr) {
                continue;
            }
            const auto image = selected_support(catalog, patch.patch).image;
            auto* site = implementation_->find(image, operation->location.rva);
            std::unique_ptr<HookSite> new_site;
            if (site == nullptr) {
                // The first surviving participant prepares the physical site and its exclusive native claim.
                auto created = prepare_absent_site(target, image, record, transaction);
                if (!created) {
                    prepared->clear_original_bindings();
                    return std::unexpected(created.error());
                }
                new_site = std::move(*created);
                site = new_site.get();
                site->participants.reserve(1);
                site->snapshots.reserve(1);
                claims.push_back({.patch = patch.patch,
                                  .image = image,
                                  .rva = operation->location.rva,
                                  .size = operation->overwrite_size,
                                  .access = planning::ClaimAccess::Write,
                                  .operation_index = record.index});
                if (operation->kind != FC_HOOK_DIRECT_CALL_SITE) {
                    auto effect = transaction.add_effect(
                        {.context = site, .apply = &enable_physical, .restore = &disable_physical},
                        operation_name(*operation));
                    if (!effect) {
                        prepared->clear_original_bindings();
                        return std::unexpected(hook_failure(effect.error(), operation_name(*operation)));
                    }
                }
            } else {
                // Reserve the successful append before the Commit phase; capacity growth exposes no participant.
                site->participants.reserve(site->participants.size() + 1);
                site->snapshots.reserve(site->snapshots.size() + 1);
            }

            // Retain the contribution now, but defer snapshots until the Prepare callback and final revalidation.
            auto contribution = participant(patch.patch, *operation);
            prepared->items.push_back({.site = site,
                                       .participant = contribution,
                                       .operation = operation_name(*operation),
                                       .new_site = std::move(new_site)});
        }
    } catch (...) {
        prepared->clear_original_bindings();
        return std::unexpected(
            hook_failure("Shared hook preparation could not allocate bounded publication state", "Prepare hooks"));
    }
    return HookPreparation{std::move(prepared)};
}

std::expected<void, planning::FailureReason> HookRegistry::bind_originals(HookPreparation& preparation) {
    if (preparation.implementation_ == nullptr) {
        return {};
    }
    // Bind the complete owner set together; an ABI failure clears earlier bindings before returning to cleanup.
    for (auto& item : preparation.implementation_->items) {
        if (item.participant.observer || item.participant.bind_original == nullptr) {
            continue;
        }
        try {
            item.participant.bind_original(item.participant.original_context, item.site->original);
            item.original_bound = true;
        } catch (...) {
            preparation.implementation_->clear_original_bindings();
            return std::unexpected(hook_failure("Original binding threw across the native boundary", item.operation));
        }
    }
    return {};
}

std::expected<void, planning::FailureReason> HookRegistry::finalize_patch(HookPreparation& preparation,
                                                                          const catalog::Catalog& catalog) {
    if (preparation.implementation_ == nullptr) {
        return {};
    }
    try {
        // Build the complete replacement set together so a partial allocation failure leaves all bytes unexposed.
        for (auto& item : preparation.implementation_->items) {
            if (item.snapshot != nullptr) {
                return std::unexpected(
                    hook_failure("Shared hook snapshots were finalized more than once", "Prepare hook snapshots"));
            }
            item.snapshot = make_snapshot(catalog, *item.site, item.participant);
        }
    } catch (...) {
        return std::unexpected(hook_failure("Shared hook snapshots could not be allocated", "Prepare hook snapshots"));
    }
    return {};
}

void HookRegistry::publish(HookPreparation preparation) noexcept {
    if (preparation.implementation_ == nullptr) {
        return;
    }
    for (auto& item : preparation.implementation_->items) {
        // Finalization is an internal prerequisite; publication cannot recover after the Commit phase or allocation.
        if (item.snapshot == nullptr) {
            fatal_invariant("Shared hook publication reached the Activate phase without a finalized snapshot");
        }
        auto* site = item.site;
        // New sites join the permanent registry before their projection or snapshot can be observed by later work.
        if (item.new_site != nullptr) {
            site->projection_index = implementation_->projections.size();
            implementation_->projections.push_back(std::move(site->prepared_projection));
            implementation_->sites.push_back(std::move(item.new_site));
        }
        // Retain every backing object before the release store exposes pointers into the immutable snapshot.
        site->participants.push_back(item.participant);
        const auto* published = &item.snapshot->snapshot;
        site->snapshots.push_back(std::move(item.snapshot));

        auto& projection = implementation_->projections[site->projection_index];
        projection.has_owner = published->owner.callback != 0;
        projection.observer_count = published->observer_count;
        projection.state_size = published->total_state_size;
        // This store is the only point at which a successfully activated patch becomes callable at the site.
        std::atomic_ref{site->snapshot_slot}.store(reinterpret_cast<std::uintptr_t>(published),
                                                   std::memory_order_release);
    }
}

void HookRegistry::visit_patch_resources(catalog::PatchIndex patch, void* context,
                                         HookResourceVisitor visitor) const noexcept {
    if (visitor == nullptr) {
        return;
    }
    for (const auto& site : implementation_->sites) {
        // The first participant is the patch whose attempt created and retained this physical site.
        if (!site->participants.empty() && site->participants.front().patch == patch) {
            visitor(context, resource_view(*site));
        }
    }
}

} // namespace fc::patching
