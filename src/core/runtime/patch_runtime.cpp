#include "patch_runtime.hpp"

#include <type_traits>
#include <utility>

namespace fc::runtime {

static_assert(std::is_nothrow_move_constructible_v<InstalledPatchRecord>);
static_assert(std::is_nothrow_move_constructible_v<RetainedFailureRecord>);

void AwaitedImageSet::reset_from_waiting(const catalog::Catalog& catalog,
                                         const planning::PatchWorkSet& patches) noexcept {
    // Patch selection fixes one support ordinal, leaving the plugin catalog as image authority for each work bit.
    mask_ = 0;
    for (const auto& record : patches.records()) {
        if (record.state != planning::PatchState::WaitingForImage) {
            continue;
        }
        const auto& patch = catalog.patch(record.patch);
        const auto image = patch.supports[*patch.selected_support].image;
        mask_ = static_cast<std::uint8_t>(mask_ | bit(image));
    }
}

bool AwaitedImageSet::contains(FC_TargetImage image) const noexcept {
    const auto image_bit = bit(image);
    return image_bit != 0 && (mask_ & image_bit) != 0;
}

void AwaitedImageSet::remove(FC_TargetImage image) noexcept {
    // Removal is terminal for this process; no public or internal path adds a bit after initial resolution.
    mask_ = static_cast<std::uint8_t>(mask_ & ~bit(image));
}

bool AwaitedImageSet::empty() const noexcept {
    return mask_ == 0;
}

std::uint8_t AwaitedImageSet::bit(FC_TargetImage image) noexcept {
    if (image < FC_IMAGE_GAME || image > FC_IMAGE_GALAXY_PEER) {
        return 0;
    }
    return static_cast<std::uint8_t>(1U << (image - FC_IMAGE_GAME));
}

InstallationAttempt::InstallationAttempt(patching::PatchTransaction transaction,
                                         std::vector<planning::MemoryClaim> claims) noexcept
    : transaction_(std::move(transaction)), claims_(std::move(claims)) {}

patching::PatchTransaction& InstallationAttempt::transaction() noexcept {
    return transaction_;
}

const patching::PatchTransaction& InstallationAttempt::transaction() const noexcept {
    return transaction_;
}

const patching::HookPreparation& InstallationAttempt::hook_preparation() const noexcept {
    return hooks_;
}

std::span<const planning::MemoryClaim> InstallationAttempt::claims() const noexcept {
    return claims_;
}

std::vector<planning::MemoryClaim>& InstallationAttempt::mutable_claims() noexcept {
    return claims_;
}

void InstallationAttempt::set_hook_preparation(patching::HookPreparation preparation) noexcept {
    hooks_ = std::move(preparation);
}

std::expected<void, planning::FailureReason> InstallationAttempt::bind_originals(patching::HookRegistry& registry) {
    return registry.bind_originals(hooks_);
}

std::expected<void, planning::FailureReason> InstallationAttempt::finalize_hooks(patching::HookRegistry& registry,
                                                                                 const catalog::Catalog& catalog) {
    return registry.finalize_patch(hooks_, catalog);
}

std::expected<void, planning::FailureReason>
InstallationAttempt::finalize_interfaces(InterfaceRouter& router, const planning::SubmittedPlan& plan) {
    // Keep the existing attempt owner unchanged unless every binding record is copied successfully.
    auto prepared = router.prepare(plan.operations);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    interfaces_ = std::move(*prepared);
    return {};
}

void InstallationAttempt::clear_original_bindings() noexcept {
    hooks_.clear_original_bindings();
}

FC_TraceCreateResult InstallationAttempt::create_trace(TraceSession& traces, catalog::PatchIndex patch,
                                                       const FC_TraceDefinition* definition,
                                                       FC_TraceHandle* output) noexcept {
    // Construct the attempt token first so a successfully allocated channel always has a cleanup owner.
    try {
        traces_.emplace_back();
    } catch (...) {
        if (output != nullptr) {
            *output = nullptr;
        }
        return FC_TRACE_REJECTED;
    }
    // Disabled or rejected requests remove the empty token; created requests retain it through the Commit phase.
    const auto result = traces.prepare_channel(patch, definition, traces_.back(), output);
    if (result != FC_TRACE_CREATED) {
        traces_.pop_back();
    }
    return result;
}

void InstallationAttempt::arm_traces(TraceSession& traces) noexcept {
    for (auto& channel : traces_) {
        traces.arm(channel);
    }
}

void InstallationAttempt::publish_hooks(patching::HookRegistry& registry) noexcept {
    registry.publish(std::move(hooks_));
}

void InstallationAttempt::publish_interfaces(InterfaceRouter& router, catalog::PatchIndex consumer) noexcept {
    router.connect_consumer(consumer, std::move(interfaces_));
}

void InstallationAttempt::release_prepared_resources() noexcept {
    // Release hook entries and native allocations before discarding claims that describe their prepared ranges.
    hooks_ = {};
    interfaces_ = {};
    traces_.clear();
    transaction_.release_prepared_resources();
    claims_.clear();
}

PatchRuntimeState::PatchRuntimeState(targets::RecognizedTarget owned_target, catalog::Catalog owned_catalog)
    : target(std::move(owned_target)), catalog(std::move(owned_catalog)), patches(catalog) {}

planning::ValidationBaseline PatchRuntimeState::validation_baseline() const noexcept {
    return {.installed_claims = installed_claims,
            .blocked_claims = blocked_claims,
            .installed_hook_sites = hooks.installed_sites()};
}

} // namespace fc::runtime
