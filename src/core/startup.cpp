#include "startup.hpp"

#include "patching/patching.hpp"

#include <algorithm>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fusioncutter {
namespace {

struct PatchCandidate {
    const catalog::CatalogEntry* entry;
    const PatchVariant* variant;
    PatchInstance instance;
    std::optional<PreparedPatchPlan> plan;
    bool eligible{true};
};

struct PendingPatch {
    const catalog::CatalogEntry* entry;
    const PatchVariant* variant;
    ResolvedSettings settings;
};

struct ActivePatch {
    PatchInstance instance;
    std::optional<PreparedPatchPlan> plan;
    Updatable* updatable;
};

[[nodiscard]] OutcomeReason startup_error(std::string message, std::string operation = {}) {
    return {std::move(message),
            operation.empty() ? std::optional<std::string>{} : std::optional<std::string>{std::move(operation)},
            {}};
}

[[nodiscard]] Patch* planned_patch(PatchInstance& instance) noexcept {
    if (auto* patch = std::get_if<std::unique_ptr<Patch>>(&instance)) {
        return patch->get();
    }
    return nullptr;
}

[[nodiscard]] bool has_instance(const PatchInstance& instance) noexcept {
    return std::visit(
        [](const auto& patch) {
            return patch != nullptr;
        },
        instance);
}

[[nodiscard]] RuntimePatch* runtime_patch(PatchInstance& instance) noexcept {
    if (auto* patch = planned_patch(instance)) {
        return dynamic_cast<RuntimePatch*>(patch);
    }
    return nullptr;
}

[[nodiscard]] RuntimeOnlyPatch* runtime_only_patch(PatchInstance& instance) noexcept {
    if (auto* patch = std::get_if<std::unique_ptr<RuntimeOnlyPatch>>(&instance)) {
        return patch->get();
    }
    return nullptr;
}

[[nodiscard]] Updatable* updatable_patch(PatchInstance& instance) noexcept {
    if (auto* patch = planned_patch(instance)) {
        return dynamic_cast<Updatable*>(patch);
    }
    return dynamic_cast<Updatable*>(runtime_only_patch(instance));
}

[[nodiscard]] StatusContributor* status_contributor(PatchInstance& instance) noexcept {
    if (auto* patch = runtime_patch(instance)) {
        return dynamic_cast<StatusContributor*>(patch);
    }
    return dynamic_cast<StatusContributor*>(runtime_only_patch(instance));
}

[[nodiscard]] std::expected<void, OutcomeReason> prepare_runtime(PatchInstance& instance) {
    if (auto* patch = runtime_patch(instance)) {
        return patch->prepare_runtime();
    }
    if (auto* patch = runtime_only_patch(instance)) {
        return patch->prepare_runtime();
    }
    return {};
}

void enable_runtime(PatchInstance& instance) noexcept {
    if (auto* runtime = runtime_patch(instance)) {
        runtime->enable_runtime();
    } else if (auto* runtime_only = runtime_only_patch(instance)) {
        runtime_only->enable_runtime();
    }
}

void disable_runtime(PatchInstance& instance) noexcept {
    if (auto* runtime = runtime_patch(instance)) {
        runtime->disable_runtime();
    } else if (auto* runtime_only = runtime_only_patch(instance)) {
        runtime_only->disable_runtime();
    }
}

[[nodiscard]] const TargetContext* find_image(std::span<const TargetContext> images, TargetImage identity) noexcept {
    const auto found = std::ranges::find(images, identity, [](const auto& target) {
        return target.image.identity;
    });
    return found == images.end() ? nullptr : &*found;
}

[[nodiscard]] const config::PatchToggle* find_toggle(const config::Configuration& configuration,
                                                     PatchId patch_id) noexcept {
    const auto toggles = configuration.patch_toggles();
    const auto found = std::ranges::find(toggles, patch_id, &config::PatchToggle::patch_id);
    return found == toggles.end() ? nullptr : &*found;
}

[[nodiscard]] std::expected<ResolvedSettings, OutcomeReason>
resolve_settings(const catalog::SelectedPatch& selected, const config::Configuration& configuration) {
    if (selected.entry->definition.configurable) {
        return configuration.resolve_settings(selected.entry->id);
    }
    return settings_for_variant(selected.entry->definition, *selected.variant).make_defaults();
}

[[nodiscard]] std::expected<PatchCandidate, OutcomeReason>
build_candidate(const catalog::SelectedPatch& selected, ResolvedSettings settings, const TargetContext& target) {
    auto instance = selected.variant->factory.construct(std::move(settings), target);
    if (!has_instance(instance)) {
        return std::unexpected(startup_error("patch factory returned no patch instance", "Construct patch"));
    }

    std::optional<PreparedPatchPlan> prepared_plan;
    if (auto* patch = planned_patch(instance)) {
        PatchPlan plan(selected.entry->id, target.image);
        patch->build_plan(plan);
        auto prepared = PreparedPatchPlan::prepare(std::move(plan));
        if (!prepared.has_value()) {
            return std::unexpected(std::move(prepared.error()));
        }
        prepared_plan.emplace(std::move(*prepared));
    }

    return PatchCandidate{selected.entry, selected.variant, std::move(instance), std::move(prepared_plan)};
}

} // namespace

class StartupState::Impl {
  public:
    explicit Impl(catalog::Catalog catalog) : catalog(std::move(catalog)) {}

    [[nodiscard]] PatchResult& result(PatchId patch_id) {
        const auto found = std::ranges::find(results, patch_id, &PatchResult::patch_id);
        return *found;
    }

    [[nodiscard]] const PatchResult& result(PatchId patch_id) const {
        const auto found = std::ranges::find(results, patch_id, &PatchResult::patch_id);
        return *found;
    }

    void set_result(PatchId patch_id, PatchOutcome outcome, std::optional<OutcomeReason> reason = {}) {
        auto& patch_result = result(patch_id);
        patch_result.outcome = outcome;
        patch_result.reason = std::move(reason);
    }

    void finish(PatchId patch_id) {
        std::erase(unfinished, patch_id);
    }

    [[nodiscard]] std::optional<OutcomeReason> unavailable_dependency(const catalog::CatalogEntry& entry,
                                                                      std::span<const PatchId> ready,
                                                                      bool waiting_patch) const {
        for (const auto dependency : entry.definition.depends_on) {
            if (std::ranges::contains(ready, dependency)) {
                continue;
            }

            const auto& dependency_result = result(dependency);
            if (dependency_result.outcome == PatchOutcome::Installed) {
                continue;
            }
            if (waiting_patch && dependency_result.outcome == PatchOutcome::WaitingForImage) {
                continue;
            }

            return OutcomeReason{"required patch '" + std::string(dependency) + "' is not installed",
                                 "Resolve dependencies", dependency};
        }
        return {};
    }

    void set_failed(const catalog::SelectedPatch& selected, OutcomeReason reason) {
        set_result(selected.entry->id, PatchOutcome::Failed, std::move(reason));
        ++status_revision;
        finish(selected.entry->id);
        if (selected.variant->failure_policy != StartupFailurePolicy::StartupRequired) {
            return;
        }

        const auto& failure = *result(selected.entry->id).reason;
        initialization = {
            InitializationOutcome::Fatal,
            OutcomeReason{"startup-required patch '" + std::string(selected.entry->id) +
                              "' could not be installed: " + failure.message,
                          failure.operation, failure.related_patch},
        };
    }

    void set_skipped(const catalog::SelectedPatch& selected, OutcomeReason reason) {
        set_result(selected.entry->id, PatchOutcome::Skipped, std::move(reason));
        ++status_revision;
        finish(selected.entry->id);
        if (selected.variant->failure_policy != StartupFailurePolicy::StartupRequired) {
            return;
        }

        const auto& failure = *result(selected.entry->id).reason;
        initialization = {
            InitializationOutcome::Fatal,
            OutcomeReason{"startup-required patch '" + std::string(selected.entry->id) +
                              "' has an unavailable dependency: " + failure.message,
                          failure.operation, failure.related_patch},
        };
    }

    void mark_unfinished(std::string message, std::string operation, PatchId cause) {
        for (const auto patch_id : unfinished) {
            set_result(patch_id, PatchOutcome::Skipped,
                       OutcomeReason{message, operation,
                                     cause.empty() ? std::optional<PatchId>{} : std::optional<PatchId>{cause}});
        }
        unfinished.clear();
        pending.clear();
        ++status_revision;
    }

    void retain(PatchCandidate candidate, bool enabled) {
        auto* updatable = enabled ? updatable_patch(candidate.instance) : nullptr;
        if (auto* contributor = enabled ? status_contributor(candidate.instance) : nullptr; contributor != nullptr) {
            contributors.push_back({candidate.entry->definition.name, contributor});
        }
        active.push_back({std::move(candidate.instance), std::move(candidate.plan), updatable});
    }

    [[nodiscard]] bool install(PatchCandidate& candidate, const catalog::SelectedPatch& selected) {
        if (auto prepared = prepare_runtime(candidate.instance); !prepared.has_value()) {
            auto reason = std::move(prepared.error());
            if (!reason.operation.has_value()) {
                reason.operation = "Prepare patch runtime";
            }
            set_failed(selected, std::move(reason));
            return false;
        }

        if (candidate.plan.has_value()) {
            auto committed = candidate.plan->commit();
            if (!committed.has_value()) {
                disable_runtime(candidate.instance);
                auto failure = std::move(committed.error());
                set_failed(selected, std::move(failure.reason));
                if (failure.rollback_failed) {
                    const auto patch_id = selected.entry->id;
                    retain(std::move(candidate), false);
                    initialization = {
                        InitializationOutcome::Fatal,
                        startup_error("patch '" + std::string(patch_id) +
                                          "' could not be rolled back safely; further installation stopped",
                                      "Roll back patch"),
                    };
                }
                return false;
            }
        }

        enable_runtime(candidate.instance);
        set_result(selected.entry->id, PatchOutcome::Installed);
        ++status_revision;
        finish(selected.entry->id);
        retain(std::move(candidate), true);
        return true;
    }

    void install_pending(PendingPatch& waiting, const TargetContext& target) {
        const catalog::SelectedPatch selected{waiting.entry, waiting.variant};
        auto candidate = build_candidate(selected, std::move(waiting.settings), target);
        if (!candidate.has_value()) {
            set_failed(selected, std::move(candidate.error()));
            return;
        }
        if (candidate->plan.has_value()) {
            if (auto reserved = reservations.reserve(*candidate->plan); !reserved.has_value()) {
                set_failed(selected, std::move(reserved.error()));
                return;
            }
        }
        static_cast<void>(install(*candidate, selected));
    }

    void update_late_images(LateImageProbe probe) {
        if (probe == nullptr || stop_late_installation) {
            return;
        }

        for (std::size_t index = 0; index < pending.size();) {
            auto& waiting = pending[index];
            if (std::ranges::any_of(waiting.entry->definition.depends_on, [&](PatchId dependency) {
                    return result(dependency).outcome == PatchOutcome::WaitingForImage;
                })) {
                ++index;
                continue;
            }
            if (auto unavailable = unavailable_dependency(*waiting.entry, {}, false); unavailable.has_value()) {
                const catalog::SelectedPatch selected{waiting.entry, waiting.variant};
                set_skipped(selected, std::move(*unavailable));
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }

            const auto image = probe(waiting.variant->layout, waiting.variant->role, waiting.variant->image);
            if (!image.has_value()) {
                const catalog::SelectedPatch selected{waiting.entry, waiting.variant};
                set_failed(selected, image.error());
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            if (!image->has_value()) {
                ++index;
                continue;
            }

            const auto& target = **image;
            if (target.layout != waiting.variant->layout || target.role != waiting.variant->role ||
                target.image.identity != waiting.variant->image) {
                const catalog::SelectedPatch selected{waiting.entry, waiting.variant};
                set_failed(selected, startup_error("late-image probe returned the wrong target", "Recognize image"));
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }

            const auto patch_id = waiting.entry->id;
            install_pending(waiting, target);
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
            if (initialization.outcome == InitializationOutcome::Fatal) {
                mark_unfinished("late-image installation stopped before this patch could be installed",
                                "Install late-image patches", patch_id);
                stop_late_installation = true;
                return;
            }
        }
    }

    catalog::Catalog catalog;
    MutationReservations reservations;
    InitializationResult initialization{InitializationOutcome::Completed, {}};
    std::vector<PatchResult> results;
    std::vector<PatchId> unfinished;
    std::vector<PendingPatch> pending;
    std::vector<ActivePatch> active;
    std::vector<ActiveStatusContributor> contributors;
    std::uint64_t status_revision{};
    bool stop_late_installation{};
};

StartupState::StartupState(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
StartupState::StartupState(StartupState&&) noexcept = default;
StartupState& StartupState::operator=(StartupState&&) noexcept = default;
StartupState::~StartupState() = default;

const InitializationResult& StartupState::initialization_result() const noexcept {
    return impl_->initialization;
}

std::span<const PatchResult> StartupState::patch_results() const noexcept {
    return impl_->results;
}

std::span<const ActiveStatusContributor> StartupState::status_contributors() const noexcept {
    return impl_->contributors;
}

std::uint64_t StartupState::status_revision() const noexcept {
    return impl_->status_revision;
}

void StartupState::update(LateImageProbe late_image_probe) noexcept {
    try {
        impl_->update_late_images(late_image_probe);
    } catch (...) {
        impl_->initialization = {
            InitializationOutcome::Fatal,
            startup_error("late-image installation ended unexpectedly", "Install late-image patches"),
        };
        impl_->mark_unfinished("late-image installation stopped before this patch could be installed",
                               "Install late-image patches", {});
        impl_->stop_late_installation = true;
    }

    for (const auto& patch : impl_->active) {
        if (patch.updatable != nullptr) {
            patch.updatable->update();
        }
    }
}

StartupState run_startup(catalog::Catalog catalog, config::Configuration configuration,
                         std::span<const TargetContext> startup_images) {
    auto impl = std::make_unique<StartupState::Impl>(std::move(catalog));
    StartupState state(std::move(impl));

    if (startup_images.empty()) {
        state.impl_->initialization = {
            InitializationOutcome::Fatal,
            startup_error("startup did not provide a recognized target image", "Bind target images"),
        };
        return state;
    }

    const auto& environment = startup_images.front();
    if (std::ranges::any_of(startup_images, [&](const auto& image) {
            return image.layout != environment.layout || image.role != environment.role ||
                   image.image.architecture != environment.image.architecture;
        })) {
        state.impl_->initialization = {
            InitializationOutcome::Fatal,
            startup_error("recognized startup images do not describe one target environment", "Bind target images"),
        };
        return state;
    }

    std::vector<catalog::PatchOverride> overrides;
    for (const auto& toggle : configuration.patch_toggles()) {
        if (!toggle.error.has_value() && toggle.override_value.has_value()) {
            overrides.push_back({toggle.patch_id, *toggle.override_value});
        }
    }
    const auto selection = catalog::select_patches(state.impl_->catalog, environment, overrides);

    state.impl_->results.reserve(state.impl_->catalog.entries().size());
    for (const auto& entry : state.impl_->catalog.entries()) {
        state.impl_->results.push_back({entry.id, entry.definition.name, PatchOutcome::NotApplicable, {}});
    }
    for (const auto* entry : selection.disabled) {
        const auto* toggle = find_toggle(configuration, entry->id);
        if (toggle != nullptr && toggle->error.has_value()) {
            state.impl_->set_result(entry->id, PatchOutcome::Failed, toggle->error);
        } else {
            state.impl_->set_result(entry->id, PatchOutcome::Disabled);
        }
    }
    for (const auto& selected : selection.install_order) {
        state.impl_->set_result(selected.entry->id, PatchOutcome::Skipped, startup_error("patch was not processed"));
        state.impl_->unfinished.push_back(selected.entry->id);
    }

    std::vector<PatchCandidate> candidates;
    std::vector<PatchId> planned;
    for (const auto& selected : selection.install_order) {
        if (const auto* toggle = find_toggle(configuration, selected.entry->id);
            toggle != nullptr && toggle->error.has_value()) {
            state.impl_->set_failed(selected, *toggle->error);
        } else if (auto unavailable = state.impl_->unavailable_dependency(
                       *selected.entry, planned, selected.variant->image_timing == ImageTiming::OneShotLate);
                   unavailable.has_value()) {
            state.impl_->set_skipped(selected, std::move(*unavailable));
        } else {
            auto settings = resolve_settings(selected, configuration);
            if (!settings.has_value()) {
                state.impl_->set_failed(selected, std::move(settings.error()));
            } else if (selected.variant->image_timing == ImageTiming::OneShotLate) {
                state.impl_->set_result(selected.entry->id, PatchOutcome::WaitingForImage);
                state.impl_->pending.push_back({selected.entry, selected.variant, std::move(*settings)});
                planned.push_back(selected.entry->id);
            } else if (const auto* target = find_image(startup_images, selected.variant->image); target == nullptr) {
                state.impl_->set_failed(
                    selected, startup_error("the patch's required startup image is not loaded", "Bind target image"));
            } else {
                auto candidate = build_candidate(selected, std::move(*settings), *target);
                if (!candidate.has_value()) {
                    state.impl_->set_failed(selected, std::move(candidate.error()));
                } else {
                    candidates.push_back(std::move(*candidate));
                    planned.push_back(selected.entry->id);
                }
            }
        }

        if (state.impl_->initialization.outcome == InitializationOutcome::Fatal) {
            state.impl_->mark_unfinished("core startup stopped before this patch could be installed", "Install patches",
                                         selected.entry->id);
            return state;
        }
    }

    std::vector<PatchId> reserved;
    for (auto& candidate : candidates) {
        const catalog::SelectedPatch selected{candidate.entry, candidate.variant};
        if (auto unavailable = state.impl_->unavailable_dependency(*candidate.entry, reserved, false);
            unavailable.has_value()) {
            candidate.eligible = false;
            state.impl_->set_skipped(selected, std::move(*unavailable));
        } else if (candidate.plan.has_value()) {
            if (auto reservation = state.impl_->reservations.reserve(*candidate.plan); !reservation.has_value()) {
                candidate.eligible = false;
                state.impl_->set_failed(selected, std::move(reservation.error()));
            } else {
                reserved.push_back(candidate.entry->id);
            }
        } else {
            reserved.push_back(candidate.entry->id);
        }

        if (state.impl_->initialization.outcome == InitializationOutcome::Fatal) {
            state.impl_->mark_unfinished("core startup stopped before this patch could be installed", "Install patches",
                                         candidate.entry->id);
            return state;
        }
    }

    for (auto& candidate : candidates) {
        if (!candidate.eligible) {
            continue;
        }

        const catalog::SelectedPatch selected{candidate.entry, candidate.variant};
        if (auto unavailable = state.impl_->unavailable_dependency(*candidate.entry, {}, false);
            unavailable.has_value()) {
            state.impl_->set_skipped(selected, std::move(*unavailable));
        } else {
            static_cast<void>(state.impl_->install(candidate, selected));
        }

        if (state.impl_->initialization.outcome == InitializationOutcome::Fatal) {
            state.impl_->mark_unfinished("core startup stopped before this patch could be installed", "Install patches",
                                         candidate.entry->id);
            return state;
        }
    }

    for (std::size_t index = 0; index < state.impl_->pending.size();) {
        auto& waiting = state.impl_->pending[index];
        if (auto unavailable = state.impl_->unavailable_dependency(*waiting.entry, {}, true); unavailable.has_value()) {
            const catalog::SelectedPatch selected{waiting.entry, waiting.variant};
            state.impl_->set_skipped(selected, std::move(*unavailable));
            state.impl_->pending.erase(state.impl_->pending.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }

    return state;
}

} // namespace fusioncutter
