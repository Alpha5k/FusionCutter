#include "patch_record.hpp"
#include "native/plan.hpp"
#include "patch_api.hpp"
#include "fatal_boundary.hpp"

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>
#include <cassert>
#include <chrono>

namespace fc::runtime {

std::string_view state_name(PatchState state) {
    switch (state) {
    case PatchState::Disabled:
        return "Disabled";
    case PatchState::Waiting:
        return "Waiting";
    case PatchState::Planning:
        return "Planning";
    case PatchState::Preparing:
        return "Preparing";
    case PatchState::Installed:
        return "Installed";
    case PatchState::Failed:
        return "Failed";
    }
    return "Failed";
}

std::string qualified_patch_id(const PatchRecord &patch) {
    if (patch.internal_factory.create) {
        return patch.resolved.owner;
    }

    return std::string{fc::detail::view(patch.plugin->admitted.definition->id)} + "." + std::string{fc::detail::view(patch.definition->id)};
}

PatchRecord::PatchRecord() = default;

PatchRecord::~PatchRecord() {
    destroy_unexposed();
}

std::uint32_t PatchRecord::failure_policy() const noexcept {
    if (selection.variant) {
        return selection.variant->failure_policy;
    }
    return definition ? definition->failure_policy : FC_FAILURE_CONTINUE;
}

namespace {

// Identifies enabled patches that configuration cannot disable, including through a group.
bool mandatory(const PluginRecord &plugin, const FC_Patch &patch) {
    if (!patch.enabled || patch.configurable) {
        return false;
    }

    const auto &definition = *plugin.admitted.definition;
    for (std::uint32_t i = 0; i != definition.group_count; ++i) {
        const auto &group = definition.groups[i];
        if (!group.configurable) {
            continue;
        }

        for (std::uint32_t j = 0; j != group.member_count; ++j) {
            if (fc::detail::equal_id(fc::detail::view(group.members[j]), fc::detail::view(patch.id))) {
                return false;
            }
        }
    }

    return true;
}

} // namespace

void PatchRecord::destroy_unexposed() noexcept {
    // Installed handlers and handlers with exposed native plans can still be reached by callbacks.
    if (state != PatchState::Installed && (!plan || !plan->exposed()) && handler.destroy) {
        fc::invoke_patch_callback([&] {
            handler.destroy(handler.context);
        });
        handler = {};
    }
}

SelectionSession resolve_patches(AdmissionResult admitted, std::span<const ImageProfile> catalog, FC_Role role) {
    SelectionSession session{catalog, role};
    session.diagnostics = std::move(admitted.diagnostics);

    for (auto &admitted_plugin : admitted.admitted) {
        // Allocate stable records before resolving selectors so failed declarations still appear in reports.
        auto plugin = std::make_unique<PluginRecord>();
        plugin->admitted = std::move(admitted_plugin);
        const auto &declaration = *plugin->admitted.definition;
        for (std::uint32_t i = 0; i != declaration.patch_count; ++i) {
            auto patch = std::make_unique<PatchRecord>();
            patch->plugin = plugin.get();
            patch->definition = &declaration.patches[i];

            auto resolution = resolve_patch(plugin->admitted, i, catalog);
            if (!resolution) {
                patch->state = PatchState::Failed;
                patch->reason = resolution.error().message;
                session.diagnostics.push_back(resolution.error());
            } else {
                patch->resolved = std::move(*resolution);
            }

            session.patches.push_back(std::move(patch));
        }
        session.plugins.push_back(std::move(plugin));
    }

    return session;
}

void configure_patches(SelectionSession &session, std::span<const ImageObservation> observations, const ConfigurationPaths &paths) {
    for (const auto &plugin : session.plugins) {
        std::vector<const ResolvedPatch *> resolved;
        for (const auto &patch : session.patches) {
            if (patch->plugin == plugin.get() && patch->resolved.definition) {
                patch->selection = evaluate_patch(patch->resolved, session.role, observations);
                resolved.push_back(&patch->resolved);
            }
        }

        // Capture settings for applicable variants once, including those waiting for a DLL to load.
        auto configuration = load_configuration(plugin->admitted, resolved, session.role, observations, paths);
        if (configuration) {
            plugin->configuration = std::move(*configuration);
        } else {
            plugin->failure = configuration.error();
            session.diagnostics.push_back(configuration.error());

            // Apply startup policy when broken configuration prevents a mandatory compatible patch from starting.
            for (const auto &patch : session.patches) {
                if (patch->plugin == plugin.get() && mandatory(*plugin, *patch->definition) && patch->resolved.definition &&
                    patch->selection.state != SelectionState::Unavailable) {
                    patch->selected_for_startup = true;
                    session.fatal |= patch->failure_policy() == FC_FAILURE_FATAL;
                }
            }
        }
    }

    refresh_selection(session, observations);
}

void refresh_selection(SelectionSession &session, std::span<const ImageObservation> observations) {
    for (const auto &patch : session.patches) {
        if (patch->state == PatchState::Installed || patch->state == PatchState::Failed) {
            continue;
        }

        const auto &plugin = *patch->plugin;
        if (!plugin.configuration) {
            patch->state = PatchState::Failed;
            patch->reason = plugin.failure ? plugin.failure->message : "Plugin configuration is unavailable";
            continue;
        }

        // Startup controls remain fixed while image availability is reevaluated.
        const auto index = static_cast<std::size_t>(patch->definition - plugin.admitted.definition->patches);
        patch->enabled = plugin.configuration->controls[index].enabled;
        if (!patch->enabled) {
            patch->state = PatchState::Disabled;
            patch->reason.clear();
            continue;
        }

        patch->selection = evaluate_patch(patch->resolved, session.role, observations);
        patch->selected_for_startup |= patch->selection.state != SelectionState::Unavailable;
        patch->reason = patch->selection.reasons.empty() ? "" : patch->selection.reasons.front().message;
        switch (patch->selection.state) {
        case SelectionState::Ready:
            patch->state = PatchState::Planning;
            break;
        case SelectionState::Waiting:
            patch->state = PatchState::Waiting;
            break;
        case SelectionState::Unavailable:
            patch->state = PatchState::Failed;
            break;
        }
    }

    // Apply provider failures and waiting states after every patch has refreshed its own image selection.
    resolve_dependencies(session);
}

std::expected<void, Diagnostic> construct_handler(PatchRecord &patch, const FC_Host *host) {
    const auto owner = qualified_patch_id(patch);
    assert(patch.state == PatchState::Planning && (patch.internal_factory.create || patch.selection.variant));
    auto factory = patch.internal_factory;
    std::vector<FC_Value> values;
    if (!factory.create) {
        const auto &variant = *patch.selection.variant;
        factory = variant.factory;
        const auto &configuration = *patch.plugin->configuration;
        const auto found = std::ranges::find(configuration.variants, &variant, &CapturedVariantSettings::definition);
        assert(found != configuration.variants.end());

        // Give the plugin factory temporary views of its captured settings for the synchronous create() call.
        values.reserve(found->values.size());
        for (const auto &value : found->values) {
            values.push_back(value.view());
        }
    }

    Diagnostic error{owner, "Handler construction failed"};
    std::uint32_t result{};
    fc::invoke_patch_callback([&] {
        result = factory.create(factory.context, &patch, host, values.data(), static_cast<std::uint32_t>(values.size()), &patch.handler,
                                {&error, &receive_diagnostic});
    });

    // Mark Failed before calling any supplied destroy callback while the partial handler remains unexposed.
    if (!result || !patch.handler.context || !patch.handler.destroy) {
        patch.state = PatchState::Failed;
        patch.destroy_unexposed();
        patch.reason = error.message;
        return std::unexpected(std::move(error));
    }

    return {};
}

void enforce_startup_policy(const SelectionSession &session) {
    if (!session.fatal) {
        return;
    }

    for (const auto &patch : session.patches) {
        if (patch->state == PatchState::Failed && patch->selected_for_startup && patch->failure_policy() == FC_FAILURE_FATAL) {
            fc::fatal_invariant("Patch '" + qualified_patch_id(*patch) + "' failed during startup: " + patch->reason);
        }
    }

    fc::fatal_invariant("A required patch failed during startup");
}

} // namespace fc::runtime
