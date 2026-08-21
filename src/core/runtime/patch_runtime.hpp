#pragma once

#include "../catalog/catalog_types.hpp"
#include "../patching/hook_registry.hpp"
#include "../patching/patch_transaction.hpp"
#include "../planning/planning_types.hpp"
#include "../targets/recognition.hpp"
#include "interface_router.hpp"
#include "../reporting/tracing.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace fc::runtime {

using InstalledRecordIndex = std::uint32_t;

// Retains only the closed set of physical images still needed by WaitingForImage work records.
class AwaitedImageSet final {
  public:
    // Rebuilds the small mask after initial resolution; later transitions only remove bits.
    void reset_from_waiting(const catalog::Catalog& catalog, const planning::PatchWorkSet& patches) noexcept;
    [[nodiscard]] bool contains(FC_TargetImage image) const noexcept;
    void remove(FC_TargetImage image) noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    [[nodiscard]] static std::uint8_t bit(FC_TargetImage image) noexcept;

    std::uint8_t mask_{};
};

// Successful ownership destroys the plugin instance before native resources that its destructor may still inspect.
struct InstalledPatchRecord {
    catalog::PatchIndex patch;
    patching::NativePatchResources resources;
    planning::PatchInstance instance;
};

// Owns a patch attempt through success, safe failure before exposure, or retained failure after exposure.
class InstallationAttempt final {
  public:
    InstallationAttempt(patching::PatchTransaction transaction, std::vector<planning::MemoryClaim> claims) noexcept;
    InstallationAttempt(const InstallationAttempt&) = delete;
    InstallationAttempt& operator=(const InstallationAttempt&) = delete;
    InstallationAttempt(InstallationAttempt&&) noexcept = default;
    InstallationAttempt& operator=(InstallationAttempt&&) noexcept = default;

    [[nodiscard]] patching::PatchTransaction& transaction() noexcept;
    [[nodiscard]] const patching::PatchTransaction& transaction() const noexcept;
    // Crash publication reads prepared executable ranges without taking ownership from a retained attempt.
    [[nodiscard]] const patching::HookPreparation& hook_preparation() const noexcept;
    [[nodiscard]] std::span<const planning::MemoryClaim> claims() const noexcept;
    [[nodiscard]] std::vector<planning::MemoryClaim>& mutable_claims() noexcept;

    // Retains unpublished physical hook work until the attempt publishes it or fails.
    void set_hook_preparation(patching::HookPreparation preparation) noexcept;
    // Original handles become visible to the Prepare callback after their native inputs pass revalidation.
    [[nodiscard]] std::expected<void, planning::FailureReason> bind_originals(patching::HookRegistry& registry);
    // Snapshot allocation waits until the Prepare callback and structural revalidation have succeeded.
    [[nodiscard]] std::expected<void, planning::FailureReason> finalize_hooks(patching::HookRegistry& registry,
                                                                              const catalog::Catalog& catalog);
    // Route copies are finalized beside hook snapshots so activation performs no fallible construction.
    [[nodiscard]] std::expected<void, planning::FailureReason> finalize_interfaces(InterfaceRouter& router,
                                                                                   const planning::SubmittedPlan& plan);
    // Withdraws plugin-owned original slots before failure cleanup destroys the instance that contains them.
    void clear_original_bindings() noexcept;
    // Retains each accepted channel under this attempt until activation transfers or failure releases it.
    [[nodiscard]] FC_TraceCreateResult create_trace(TraceSession& traces, catalog::PatchIndex patch,
                                                    const FC_TraceDefinition* definition,
                                                    FC_TraceHandle* output) noexcept;
    void arm_traces(TraceSession& traces) noexcept;
    // These moves after the Commit phase publish storage reserved before native exposure.
    void publish_hooks(patching::HookRegistry& registry) noexcept;
    void publish_interfaces(InterfaceRouter& router, catalog::PatchIndex consumer) noexcept;

    // Resource release is legal only before exposure and after the caller has destroyed the plugin instance.
    void release_prepared_resources() noexcept;

  private:
    patching::PatchTransaction transaction_;
    std::vector<planning::MemoryClaim> claims_;
    patching::HookPreparation hooks_;
    InterfacePreparation interfaces_;
    std::vector<PreparedTraceChannel> traces_;
};

// Retained attempts pin callback state and every referenced allocation even when entry bytes were restored.
struct RetainedFailureRecord {
    catalog::PatchIndex patch;
    patching::RollbackResult rollback;
    InstallationAttempt attempt;
    // Declared after the attempt so plugin destruction runs while every retained framework resource is still alive.
    planning::PatchInstance instance;
    std::size_t blocked_claim_begin{};
    std::size_t blocked_claim_count{};
};

// Owns mutable installation state after target recognition and plugin admission have completed.
// Field order is lifetime policy: installed and failed instances disappear before plugin code and target images.
struct PatchRuntimeState {
    PatchRuntimeState(targets::RecognizedTarget target, catalog::Catalog catalog);
    PatchRuntimeState(const PatchRuntimeState&) = delete;
    PatchRuntimeState& operator=(const PatchRuntimeState&) = delete;
    PatchRuntimeState(PatchRuntimeState&&) = delete;
    PatchRuntimeState& operator=(PatchRuntimeState&&) = delete;

    // Late validation consumes one immutable projection of all successful and retained native authority.
    [[nodiscard]] planning::ValidationBaseline validation_baseline() const noexcept;

    targets::RecognizedTarget target;
    catalog::Catalog catalog;
    planning::PatchWorkSet patches;
    std::vector<InstalledPatchRecord> installed_patches;
    std::vector<planning::MemoryClaim> installed_claims;
    std::vector<planning::MemoryClaim> blocked_claims;
    std::vector<RetainedFailureRecord> retained_failures;
    // Dense indexes avoid publishing raw vector pointers and stay sorted by patch ID across startup and late joins.
    std::vector<InstalledRecordIndex> update_order;
    patching::HookRegistry hooks;
    InterfaceRouter interfaces;
    AwaitedImageSet awaited_images;
};

} // namespace fc::runtime
