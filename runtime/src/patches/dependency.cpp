#include "dependency.hpp"
#include "patch_record.hpp"

#include <fc/base.hpp>

#include <algorithm>
#include <functional>
#include <ranges>
#include <utility>

namespace fc::runtime {

namespace {

// Returns the first ASCII case-insensitive override match, or nullptr; a returned pointer borrows the supplied span.
const ControlOverride *find_override(std::span<const ControlOverride> overrides, std::string_view id) {
    const auto found = std::ranges::find_if(overrides, [&](const auto &value) {
        return fc::detail::equal_id(value.local_id, id);
    });
    return found == overrides.end() ? nullptr : &*found;
}

// Adds the referenced patch or all group members as providers; unresolved references fail the consumer.
void add_reference(SelectionSession &session, PatchRecord &consumer, FC_Text text) {
    // Unqualified IDs refer to this plugin; Plugin.Patch and Plugin.Group select another plugin explicitly.
    const auto reference = fc::detail::view(text);
    const auto dot = reference.find('.');
    const auto plugin_id =
        dot == std::string_view::npos ? fc::detail::view(consumer.plugin->admitted.definition->id) : reference.substr(0, dot);
    const auto local_id = dot == std::string_view::npos ? reference : reference.substr(dot + 1);

    const auto plugin = std::ranges::find_if(session.plugins, [&](const auto &value) {
        return fc::detail::equal_id(fc::detail::view(value->admitted.definition->id), plugin_id);
    });

    // Keep the first dependency failure when several references cannot be resolved.
    auto fail = [&](std::string detail) {
        if (consumer.state != PatchState::Failed) {
            consumer.state = PatchState::Failed;
            consumer.reason = "dependency '" + std::string{reference} + "' " + std::move(detail);
        }
    };

    if (plugin == session.plugins.end()) {
        fail("refers to a missing plugin");
        return;
    }

    // Add a matching patch once; return false only when no patch matches so the ID can be checked as a group.
    auto add_patch = [&](std::string_view id) {
        const auto patch = std::ranges::find_if(session.patches, [&](const auto &value) {
            return value->plugin == plugin->get() && fc::detail::equal_id(fc::detail::view(value->definition->id), id);
        });
        if (patch == session.patches.end()) {
            return false;
        }
        if (!std::ranges::contains(consumer.providers, patch->get())) {
            consumer.providers.push_back(patch->get());
        }
        return true;
    };

    if (add_patch(local_id)) {
        return;
    }

    const auto &declaration = *(*plugin)->admitted.definition;
    const auto groups = std::span{declaration.groups, declaration.group_count};
    const auto group = std::ranges::find_if(groups, [&](const auto &value) {
        return fc::detail::equal_id(fc::detail::view(value.id), local_id);
    });
    if (group == groups.end()) {
        fail("does not match a patch or group");
        return;
    }

    // Depending on a group requires every member; declaration validation has already checked their IDs.
    for (const auto member : std::span{group->members, group->member_count}) {
        add_patch(fc::detail::view(member));
    }
}

} // namespace

std::string_view control_source_name(ControlSource source) noexcept {
    switch (source) {
    case ControlSource::Default:
        return "patch default";
    case ControlSource::Patch:
        return "patch INI";
    case ControlSource::Group:
        return "group";
    }
    return "patch default";
}

std::vector<PatchControl> resolve_controls(const AdmittedPlugin &plugin, std::span<const ControlOverride> overrides) {
    const auto &declaration = *plugin.definition;
    std::vector<PatchControl> result;
    result.reserve(declaration.patch_count);

    for (const auto &patch : std::span{declaration.patches, declaration.patch_count}) {
        // A configurable patch uses its own setting or default, regardless of group configuration.
        bool enabled = patch.enabled != 0;
        auto source = ControlSource::Default;
        if (patch.configurable) {
            if (const auto *value = find_override(overrides, fc::detail::view(patch.id))) {
                enabled = value->enabled;
                source = ControlSource::Patch;
            }
        } else {
            // Otherwise a configurable group can supply either its configured value or its declared default.
            for (const auto &group : std::span{declaration.groups, declaration.group_count}) {
                if (group.configurable && std::ranges::any_of(std::span{group.members, group.member_count}, [&](FC_Text member) {
                        return fc::detail::equal_id(fc::detail::view(member), fc::detail::view(patch.id));
                    })) {
                    const auto *value = find_override(overrides, fc::detail::view(group.id));
                    enabled = value ? value->enabled : group.enabled != 0;
                    source = ControlSource::Group;
                    break;
                }
            }
        }

        result.push_back({enabled, source});
    }

    return result;
}

void resolve_dependencies(SelectionSession &session) {
    // Rebuild dependencies for pending patches, including variant requirements once an image selects that variant.
    for (const auto &patch : session.patches) {
        if (!patch->enabled || patch->state == PatchState::Installed || patch->state == PatchState::Failed) {
            continue;
        }

        patch->providers.clear();
        for (const auto reference : std::span{patch->definition->depends_on, patch->definition->dependency_count}) {
            add_reference(session, *patch, reference);
        }
        if (patch->selection.state == SelectionState::Ready) {
            const auto &variant = *patch->selection.variant;
            for (const auto reference : std::span{variant.depends_on, variant.dependency_count}) {
                add_reference(session, *patch, reference);
            }
        }
    }

    // Track the current dependency chain to detect cycles and remember patches whose dependencies are already checked.
    std::vector<PatchRecord *> active, visited;
    // Fails cycles and propagates provider states through each dependency chain.
    std::function<void(PatchRecord &)> visit = [&](PatchRecord &patch) {
        if (patch.state == PatchState::Disabled || patch.state == PatchState::Installed || patch.state == PatchState::Failed ||
            std::ranges::contains(visited, &patch)) {
            return;
        }

        // Revisiting a patch in the current chain identifies a cycle; fail every participant with the same chain.
        const auto cycle = std::ranges::find(active, &patch);
        if (cycle != active.end()) {
            std::string chain;
            for (auto current = cycle; current != active.end(); ++current) {
                chain += qualified_patch_id(**current) + " -> ";
            }
            chain += qualified_patch_id(patch);
            for (auto current = cycle; current != active.end(); ++current) {
                (*current)->state = PatchState::Failed;
                (*current)->reason = "dependency cycle: " + chain;
            }
            return;
        }

        // Check providers first so their final waiting or failure state can be applied to this consumer.
        active.push_back(&patch);
        PatchRecord *waiting{};
        for (auto *provider : patch.providers) {
            visit(*provider);
            if (patch.state == PatchState::Failed) {
                continue;
            }
            if (provider->state == PatchState::Disabled || provider->state == PatchState::Failed) {
                patch.state = PatchState::Failed;
                patch.reason = "dependency '" + qualified_patch_id(*provider) +
                               (provider->state == PatchState::Disabled ? "' is disabled" : "' failed");
            } else if (provider->state == PatchState::Waiting && !waiting) {
                waiting = provider;
            }
        }

        // A failed dependency takes precedence over one that is only waiting for an image.
        if (patch.state != PatchState::Failed && waiting) {
            patch.state = PatchState::Waiting;
            patch.reason = "dependency '" + qualified_patch_id(*waiting) + "' is waiting: " + waiting->reason;
        }

        active.pop_back();
        visited.push_back(&patch);
    };

    for (const auto &patch : session.patches) {
        visit(*patch);
    }

    // Retain any prior fatal result; set the session's fatal flag when a selected Failed patch has Fatal policy.
    for (const auto &patch : session.patches) {
        session.fatal |= patch->state == PatchState::Failed && patch->selected_for_startup && patch->failure_policy() == FC_FAILURE_FATAL;
    }
}

} // namespace fc::runtime
