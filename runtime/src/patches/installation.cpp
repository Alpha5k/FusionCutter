#include "installation.hpp"

#include "core_runtime.hpp"
#include "fatal_boundary.hpp"
#include "native/hook_registry.hpp"
#include "patch_api.hpp"
#include "native/plan.hpp"
#include "reporting/crash_reporter.hpp"
#include "reporting/tracing.hpp"

#include <fc/reporting.hpp>

#include <fc/base.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <initializer_list>

namespace fc::runtime {

namespace {

// Rejects a patch and releases its startup resources, or terminates if its native changes have already taken effect.
void reject_patch(SelectionSession &session, PatchRecord &patch, Diagnostic diagnostic, InstallationServices services,
                  std::string_view stage) {
    diagnostic.owner = qualified_patch_id(patch);
    diagnostic.message = std::string{stage} + " failed: " + diagnostic.message;
    if (patch.plan && patch.plan->exposed()) {
        fc::fatal_invariant("Patch '" + diagnostic.owner + "': " + diagnostic.message, diagnostic.windows_error);
    }
    diagnostic.message += "; patch will not run";
    patch.reason = diagnostic.message;

    // Mark the handler unavailable before destroying it so retained consumer callbacks refuse access.
    patch.state = PatchState::Failed;
    PatchCallbackScope scope{&patch};
    patch.destroy_unexposed();

    // Destroy the handler before releasing registrations and storage it may use during cleanup.
    if (patch.runtime) {
        patch.runtime->game().discard(patch);
        patch.runtime->multiplayer().discard(patch);
        patch.runtime->lobby().discard(patch);
    }
    if (patch.api) {
        patch.api->discard();
    }
    if (services.traces) {
        services.traces->discard(&patch);
    }
    patch.plan.reset();

    session.diagnostics.push_back(std::move(diagnostic));

    if (patch.selected_for_startup && patch.failure_policy() == FC_FAILURE_FATAL) {
        session.fatal = true;
        enforce_startup_policy(session);
    }
}

// Returns a diagnostic for the first failed or disabled provider, or nullopt when all are usable.
// When require_installed is true, any provider that has not installed and activated is also rejected.
std::optional<Diagnostic> unavailable_provider(const PatchRecord &patch, bool require_installed) {
    for (const auto *provider : patch.providers) {
        if (provider->state == PatchState::Failed || provider->state == PatchState::Disabled) {
            return Diagnostic{qualified_patch_id(patch), "Required patch '" + qualified_patch_id(*provider) +
                                                             (provider->state == PatchState::Disabled ? "' is disabled" : "' failed")};
        }
        if (require_installed && provider->state != PatchState::Installed) {
            return Diagnostic{qualified_patch_id(patch),
                              "Required patch '" + qualified_patch_id(*provider) + "' has not completed installation and activation"};
        }
    }
    return std::nullopt;
}

// Copies native operation ranges and details before installation; owner names use the earlier copied patch identities.
void publish_prepared(CrashReporter *crash, PatchRecord &patch) {
    if (!crash) {
        return;
    }

    std::vector<CrashAnnotation> annotations;
    for (const auto &write : patch.plan->writes()) {
        CrashAnnotation annotation{CrashAnnotationKind::Mutation, &patch, write.address, write.bytes.size()};
        annotation.detail = write.location.name;
        annotations.push_back(std::move(annotation));
    }

    // Include hook sites, generated bridges and allocated resources so faults can be attributed to their patch.
    for (const auto &operation : patch.plan->operations()) {
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, HookParticipant>) {
                    const auto &image = patch.plan->images()[value.location.image];
                    CrashAnnotation annotation{CrashAnnotationKind::Hook, &patch, image.base + value.location.rva, value.original.size()};
                    annotation.detail = value.location.name;
                    annotations.push_back(std::move(annotation));

                    for (const auto *bridge : {value.entry, value.invoker}) {
                        if (bridge && bridge->address()) {
                            CrashAnnotation generated{CrashAnnotationKind::NativeResource, &patch, bridge->address(), bridge->size()};
                            generated.detail = value.location.name;
                            annotations.push_back(std::move(generated));
                        }
                    }
                }

                if constexpr (requires { value.resource; }) {
                    if (value.resource) {
                        CrashAnnotation annotation{CrashAnnotationKind::NativeResource, &patch, value.resource->address,
                                                   value.resource->size};
                        if constexpr (requires { value.location; }) {
                            annotation.detail = value.location.name;
                        } else {
                            annotation.detail = "allocation";
                        }
                        annotations.push_back(std::move(annotation));

                        if (value.resource->invoker.address()) {
                            CrashAnnotation invoker{CrashAnnotationKind::NativeResource, &patch, value.resource->invoker.address(),
                                                    value.resource->invoker.size()};
                            if constexpr (requires { value.location; }) {
                                invoker.detail = value.location.name;
                            }
                            annotations.push_back(std::move(invoker));
                        }
                    }
                }
            },
            operation);
    }

    crash->publish(annotations);
}

} // namespace

std::vector<PatchRecord *> ready_patch_order(const SelectionSession &session) {
    std::vector<PatchRecord *> remaining, result;
    for (const auto &patch : session.patches) {
        if (patch->state == PatchState::Planning) {
            remaining.push_back(patch.get());
        }
    }

    // Use ASCII case-insensitive plugin and patch IDs to order unrelated patches.
    std::ranges::sort(remaining, [](const auto *a, const auto *b) {
        const auto a_plugin = fc::detail::folded_id(fc::detail::view(a->plugin->admitted.definition->id));
        const auto b_plugin = fc::detail::folded_id(fc::detail::view(b->plugin->admitted.definition->id));
        return a_plugin == b_plugin
                   ? fc::detail::folded_id(fc::detail::view(a->definition->id)) < fc::detail::folded_id(fc::detail::view(b->definition->id))
                   : a_plugin < b_plugin;
    });

    result.reserve(remaining.size());

    // Choose the first patch with no provider remaining in this batch.
    while (!remaining.empty()) {
        const auto ready = std::ranges::find_if(remaining, [&](const auto *patch) {
            return std::ranges::none_of(patch->providers, [&](const auto *provider) {
                return std::ranges::find(remaining, provider) != remaining.end();
            });
        });
        if (ready == remaining.end()) {
            fc::fatal_invariant("Resolved ready dependency order contains a cycle");
        }
        result.push_back(*ready);
        remaining.erase(ready);
    }

    return result;
}

std::expected<void, Diagnostic> collect_patch_plan(PatchRecord &patch, const FC_Host *host, std::vector<PlanImage> images,
                                                   HookRegistry *hooks) {
    try {
        PatchCallbackScope scope{&patch};
        if (patch.runtime) {
            fc::Logger{{&patch, &patch.runtime->host()}}.debug("Planning patch");
        }

        if (auto created = construct_handler(patch, host); !created) {
            return created;
        }

        patch.plan = std::make_unique<SubmittedPlan>(&patch, std::move(images), hooks);
        auto boundary = patch.plan->boundary();
        Diagnostic error{qualified_patch_id(patch), "Patch planning failed"};
        if (patch.handler.plan) {
            std::uint32_t result{};
            fc::invoke_patch_callback([&] {
                result = patch.handler.plan(patch.handler.context, &boundary, {&error, &receive_diagnostic});
            });
            if (result != 1) {
                return std::unexpected(std::move(error));
            }
        }

        // A rejected native request remains a failure even when the handler ignores its individual result.
        if (patch.plan->error()) {
            return std::unexpected(Diagnostic{qualified_patch_id(patch), patch.plan->error()->message, patch.plan->error()->windows_error});
        }
        return {};
    } catch (const Diagnostic &error) {
        return std::unexpected(error);
    } catch (const std::exception &error) {
        return std::unexpected(Diagnostic{qualified_patch_id(patch), error.what()});
    } catch (...) {
        return std::unexpected(Diagnostic{qualified_patch_id(patch), "Patch planning raised an unknown exception"});
    }
}

void install_ready_patches(SelectionSession &session, const LiveImageObservations &images, InstallationServices services) {
    auto ordered = ready_patch_order(session);

    // Install the runtime's game integrations before patches so their native changes take precedence in conflicts.
    auto position = ordered.begin();
    for (auto *owner : services.internal_native) {
        if (owner->state == PatchState::Planning) {
            position = std::next(ordered.insert(position, owner));
        }
    }

    // Include installed runtime integrations when checking a later patch's memory claims.
    std::vector<SubmittedPlan *> plans;
    std::vector<MemoryClaim> installed;
    for (const auto *owner : services.internal_native) {
        if (owner->state == PatchState::Installed && owner->plan) {
            const auto &claims = owner->plan->claims();
            installed.insert(installed.end(), claims.begin(), claims.end());
        }
    }

    // Existing patches keep their memory claims when a later batch is validated.
    for (const auto &patch : session.patches) {
        if (patch->state == PatchState::Installed && patch->plan) {
            installed.insert(installed.end(), patch->plan->claims().begin(), patch->plan->claims().end());
        }
    }

    // Construct handlers and collect all ready plans before checking conflicts or modifying the game.
    for (auto *patch : ordered) {
        if (auto dependency = unavailable_provider(*patch, false)) {
            reject_patch(session, *patch, std::move(*dependency), services, "Dependency check");
            continue;
        }

        try {
            auto selected = selected_plan_images(*patch, images);
            if (!selected) {
                throw selected.error();
            }
            if (auto planned = collect_patch_plan(*patch, services.host, std::move(*selected), services.hooks); !planned) {
                throw planned.error();
            }

            plans.push_back(patch->plan.get());
        } catch (const Diagnostic &diagnostic) {
            reject_patch(session, *patch, diagnostic, services, "Planning");
        } catch (const std::exception &exception) {
            reject_patch(session, *patch, {qualified_patch_id(*patch), exception.what()}, services, "Planning");
        } catch (...) {
            reject_patch(session, *patch, {qualified_patch_id(*patch), "Patch planning raised an unknown exception"}, services, "Planning");
        }
    }

    // Check the batch against both earlier plans in this batch and patches already installed.
    for (auto &[plan, error] : validate_plans(plans, installed)) {
        auto &owner = *plan->owner();
        reject_patch(session, owner, {qualified_patch_id(owner), error.message, error.windows_error}, services, "Plan validation");
    }

    // Prepare and install accepted patches in dependency order; a preparation failure keeps earlier conflict decisions.
    for (auto *patch : ordered) {
        if (patch->state != PatchState::Planning) {
            continue;
        }
        if (auto dependency = unavailable_provider(*patch, true)) {
            reject_patch(session, *patch, std::move(*dependency), services, "Dependency check");
            continue;
        }

        patch->state = PatchState::Preparing;
        Diagnostic error{qualified_patch_id(*patch), "the patch did not provide an error message"};
        std::string_view stage = "Preparation";
        try {
            PatchCallbackScope scope{patch};
            if (patch->runtime) {
                fc::Logger{{patch, &patch->runtime->host()}}.debug("Preparing patch");
            }

            auto prepared = patch->plan->prepare();
            if (!prepared) {
                throw Diagnostic{qualified_patch_id(*patch), prepared.error().message, prepared.error().windows_error};
            }

            // The handler can now initialize the resources allocated by its plan.
            if (patch->handler.prepare) {
                std::uint32_t result{};
                fc::invoke_patch_callback([&] {
                    result = patch->handler.prepare(patch->handler.context, {&error, &receive_diagnostic});
                });
                if (result != 1) {
                    throw error;
                }
            }

            if (patch->api && patch->api->error()) {
                throw *patch->api->error();
            }

            publish_prepared(services.crash, *patch);

            // Revalidate and apply native changes after preparation and any crash annotation publication.
            stage = "Installation";
            if (patch->runtime) {
                fc::Logger{{patch, &patch->runtime->host()}}.debug("Installing patch");
            }
            auto installed_patch = patch->plan->install();
            if (!installed_patch) {
                if (patch->plan->exposed() || installed_patch.error().unsafe) {
                    fc::fatal_invariant("Patch '" + qualified_patch_id(*patch) +
                                        "' installation failed and native bytes or memory protections may have changed: " +
                                        installed_patch.error().message);
                }
                throw Diagnostic{qualified_patch_id(*patch), installed_patch.error().message};
            }

            // Native hooks can now execute using prepared state; finish activation before exposing patch APIs and updates.
            stage = "Activation";
            if (patch->runtime) {
                fc::Logger{{patch, &patch->runtime->host()}}.debug("Activating patch");
            }
            if (patch->handler.activate) {
                fc::invoke_patch_callback([&] {
                    patch->handler.activate(patch->handler.context);
                });
            }

            patch->last_update = std::chrono::steady_clock::now();
            if (patch->definition) {
                patch->next_update = next_update_time(patch->last_update, patch->definition->update_interval_ms);
            }
            // Publish initialized handler and API state to callbacks that observe Installed on other threads.
            patch->state.store(PatchState::Installed, std::memory_order_release);
            patch->reason.clear();

            if (patch->runtime) {
                fc::Logger{{patch, &patch->runtime->host()}}.info("Installed and activated");
            }
        } catch (const Diagnostic &diagnostic) {
            reject_patch(session, *patch, diagnostic, services, stage);
        } catch (const std::exception &exception) {
            reject_patch(session, *patch, {qualified_patch_id(*patch), exception.what()}, services, stage);
        } catch (...) {
            reject_patch(session, *patch, {qualified_patch_id(*patch), "Patch setup raised an unknown exception"}, services, stage);
        }
    }
}

std::chrono::steady_clock::time_point next_update_time(std::chrono::steady_clock::time_point now, std::int64_t interval_ms) noexcept {
    using Clock = std::chrono::steady_clock;
    // Bound milliseconds before converting to the clock's finer tick representation.
    if (interval_ms > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count()) {
        return Clock::time_point::max();
    }
    const auto duration = std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds{interval_ms});
    // Compare the available time range before adding the positive duration.
    return now > Clock::time_point::max() - duration ? Clock::time_point::max() : now + duration;
}

void run_due_updates(SelectionSession &session, std::chrono::steady_clock::time_point now) {
    for (const auto &patch : session.patches) {
        if (patch->state != PatchState::Installed || !patch->handler.update || now < patch->next_update) {
            continue;
        }

        // A delayed update receives the full elapsed interval and schedules the next call from now.
        const auto elapsed = now - patch->last_update;
        patch->last_update = now;
        patch->next_update = next_update_time(now, patch->definition->update_interval_ms);

        PatchCallbackScope scope{patch.get()};
        fc::invoke_patch_callback([&] {
            patch->handler.update(patch->handler.context,
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        });
    }
}

void deliver_api_notifications(SelectionSession &session) {
    for (const auto &patch : session.patches) {
        if (patch->state != PatchState::Installed || !patch->api) {
            continue;
        }

        PatchCallbackScope scope{patch.get()};
        patch->api->deliver_available(session.diagnostics);
    }
}

} // namespace fc::runtime
