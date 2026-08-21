#include "late_images.hpp"

#include "../planning/plan_validation.hpp"
#include "../planning/resolution.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace fc::runtime {
namespace {

// Adapts the test seam that carries an explicit context to the one Windows module probe owned by the framework.
[[nodiscard]] targets::LateProbeResult production_probe(void*, const targets::RecognizedTarget& target,
                                                        FC_TargetImage image) {
    return targets::probe_late_image(target, image);
}

// Reads selected support from the plugin catalog instead of copying image identity into mutable runtime records.
[[nodiscard]] FC_TargetImage selected_image(const PatchRuntimeState& runtime,
                                            catalog::PatchIndex patch_index) noexcept {
    const auto& patch = runtime.catalog.patch(patch_index);
    return patch.supports[*patch.selected_support].image;
}

// Human-readable image identities keep runtime diagnostics useful without requiring C ABI enum knowledge.
[[nodiscard]] std::string_view image_name(FC_TargetImage image) noexcept {
    switch (image) {
    case FC_IMAGE_GAME:
        return "Game";
    case FC_IMAGE_BOOTSTRAP:
        return "Bootstrap";
    case FC_IMAGE_GALAXY_PEER:
        return "GalaxyPeer";
    default:
        return "Unknown";
    }
}

// A permanent profile rejection consumes only matching waiting records and then propagates ordinary prerequisites.
void reject_waiting_patches(PatchRuntimeState& runtime, FC_TargetImage image, const targets::LateProbeError& error) {
    for (auto& record : runtime.patches.records()) {
        if (record.state != planning::PatchState::WaitingForImage || selected_image(runtime, record.patch) != image) {
            continue;
        }
        planning::finish_inactive_patch(
            record, planning::PatchState::Skipped,
            {.message = error.message, .phase = planning::PatchPhase::Selection, .operation = error.operation});
    }
    planning::prune_unavailable_consumers(runtime.patches);
}

// Reuses the existing records and resolved settings; no selection, relationship, or configuration state is rebuilt.
void resume_waiting_patches(PatchRuntimeState& runtime, FC_TargetImage image) noexcept {
    for (auto& record : runtime.patches.records()) {
        if (record.state == planning::PatchState::WaitingForImage && selected_image(runtime, record.patch) == image) {
            record.reason.reset();
            record.state = planning::PatchState::Pending;
        }
    }
}

} // namespace

LateImageResult process_awaited_images(PatchRuntimeState& runtime, TraceSession& traces, CrashReporter& crash,
                                       LateImageServices services) {
    // The fixed array is the ABI's TargetImage order and prevents absent earlier images from blocking later probes.
    constexpr std::array images{FC_IMAGE_GAME, FC_IMAGE_BOOTSTRAP, FC_IMAGE_GALAXY_PEER};
    const auto probe = services.probe == nullptr ? &production_probe : services.probe;
    auto result = LateImageResult::Unchanged;

    for (const auto image : images) {
        if (!runtime.awaited_images.contains(image)) {
            continue;
        }

        crash.set_core_phase(CorePhase::TargetRecognition);
        auto probed = probe(services.probe_context, runtime.target, image);
        if (!probed) {
            // Unsafe or unrecognized late images are final; neither the image nor its waiting patches retry.
            services.logger.warning("Late target image '{}' was permanently rejected: {}", image_name(image),
                                    probed.error().message);
            runtime.awaited_images.remove(image);
            reject_waiting_patches(runtime, image, probed.error());
            result = LateImageResult::Changed;
            continue;
        }
        if (!*probed) {
            // Absence is the sole nonterminal outcome and intentionally has no timeout or state transition.
            continue;
        }

        runtime.awaited_images.remove(image);
        if (!runtime.target.add_late_image(std::move(**probed))) {
            const targets::LateProbeError error{"The recognized image could not occupy its reviewed stable target slot",
                                                "Recognize late target image"};
            reject_waiting_patches(runtime, image, error);
            services.logger.error("Late target image '{}' could not enter its stable target slot", image_name(image));
            result = LateImageResult::Changed;
            continue;
        }

        // The pinned owner is permanent before any callback or image pointer can observe the newly available slot.
        services.logger.info("Recognized and retained late target image '{}'", image_name(image));
        resume_waiting_patches(runtime, image);
        crash.set_core_phase(CorePhase::Validation);
        auto plan = planning::build_installation_plan(runtime.target, runtime.patches, runtime.validation_baseline(),
                                                      services.planning_logger);

        // The shared installer preserves failure mapping, installed baselines, publication, and Update callback order.
        crash.set_core_phase(CorePhase::Installation);
        if (install_ready_patches(plan, runtime, traces, crash, services.installation) == InstallationResult::Fatal) {
            return LateImageResult::Fatal;
        }
        result = LateImageResult::Changed;
    }

    crash.set_core_phase(CorePhase::Running);
    return result;
}

} // namespace fc::runtime
