#include "resolution.hpp"

#include "../catalog/definition_copy.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace fc::planning {
namespace {

// Tracks the monotonic selection closure separately from terminal PatchWorkRecord lifecycle state.
struct SelectionDecision {
    bool applicable{};
    bool fixed{};
    bool selected{};
    bool processed{};
};

[[nodiscard]] const catalog::SupportDefinitionRecord& selected_support(const catalog::Catalog& catalog,
                                                                       catalog::PatchIndex patch) noexcept {
    const auto& definition = catalog.patch(patch);
    return definition.supports[*definition.selected_support];
}

[[nodiscard]] std::span<const catalog::SettingDefinitionRecord>
effective_settings(const catalog::PatchDefinitionRecord& patch,
                   const catalog::SupportDefinitionRecord& support) noexcept {
    return support.has_settings == FC_TRUE ? std::span{support.settings} : std::span{patch.settings};
}

[[nodiscard]] std::optional<bool> parse_boolean(std::string_view text) noexcept {
    if (catalog::equal_ascii_case_insensitive(text, "true") || catalog::equal_ascii_case_insensitive(text, "on") ||
        text == "1") {
        return true;
    }
    if (catalog::equal_ascii_case_insensitive(text, "false") || catalog::equal_ascii_case_insensitive(text, "off") ||
        text == "0") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] const config::PluginConfiguration& plugin_configuration(const ResolutionInput& input,
                                                                      catalog::PluginIndex plugin) noexcept {
    return input.configuration.plugins[plugin.value];
}

[[nodiscard]] std::optional<catalog::GroupIndex> configurable_group_for(const catalog::Catalog& catalog,
                                                                        catalog::PatchIndex patch) {
    const auto& patch_id = catalog.patch(patch).id;
    for (std::size_t index = 0; index < catalog.group_count(); ++index) {
        const auto group_index = catalog::GroupIndex{static_cast<std::uint32_t>(index)};
        const auto& group = catalog.group(group_index);
        if (group.configurable == FC_TRUE && std::ranges::any_of(group.members, [&](std::string_view member) {
                return catalog::equal_ascii_case_insensitive(member, patch_id);
            })) {
            return group_index;
        }
    }
    return std::nullopt;
}

void selection_failure(PatchWorkRecord& record, std::string message, std::string operation,
                       std::optional<std::string_view> group = std::nullopt) {
    finish_inactive_patch(record, PatchState::Failed,
                          FailureReason{.message = std::move(message),
                                        .phase = PatchPhase::Selection,
                                        .operation = std::move(operation),
                                        .related_group = group});
}

// Decisions selected by configuration are fixed before relationship closure and can never be overridden.
void establish_fixed_decisions(const ResolutionInput& input, PatchWorkSet& work,
                               std::vector<SelectionDecision>& decisions) {
    for (std::size_t index = 0; index < input.catalog.patch_count(); ++index) {
        const auto patch_index = catalog::PatchIndex{static_cast<std::uint32_t>(index)};
        const auto& patch = input.catalog.patch(patch_index);
        auto& record = work.record(patch_index);
        auto& decision = decisions[index];
        if (!patch.selected_support) {
            record.state = PatchState::NotApplicable;
            continue;
        }
        decision.applicable = true;

        // A patch's own configurable toggle is authoritative and cannot later be selected through a relationship.
        const auto plugin = input.catalog.patch_plugin(patch_index);
        const auto ordinal = input.catalog.patch_ordinal(patch_index);
        const auto& patch_configuration = plugin_configuration(input, plugin).patches[ordinal];
        if (patch.configurable == FC_TRUE) {
            decision.fixed = true;
            if (!patch_configuration.toggle) {
                decision.selected = patch.enabled == FC_TRUE;
                continue;
            }
            const auto value = parse_boolean(patch_configuration.toggle->value);
            if (!value) {
                selection_failure(record, "Patch toggle is not a valid Boolean", "Resolve patch toggle");
                continue;
            }
            decision.selected = *value;
            continue;
        }

        // Otherwise a configurable containing group supplies the one inherited fixed decision for its members.
        const auto group_index = configurable_group_for(input.catalog, patch_index);
        if (group_index) {
            decision.fixed = true;
            const auto& group = input.catalog.group(*group_index);
            const auto group_plugin = input.catalog.group_plugin(*group_index);
            const auto group_ordinal = input.catalog.group_ordinal(*group_index);
            const auto& group_configuration = plugin_configuration(input, group_plugin).groups[group_ordinal];
            if (!group_configuration.toggle) {
                decision.selected = group.enabled == FC_TRUE;
                continue;
            }
            const auto value = parse_boolean(group_configuration.toggle->value);
            if (!value) {
                selection_failure(record, "Inherited group toggle is not a valid Boolean", "Resolve group toggle",
                                  group.id);
                continue;
            }
            decision.selected = *value;
            continue;
        }

        // Uncontrolled defaults seed the closure but remain eligible for later relationship selection.
        decision.selected = patch.enabled == FC_TRUE;
    }
}

template <class Function>
void for_each_relationship_target(const catalog::Catalog& catalog, std::string_view source, Function&& function) {
    if (const auto patch = catalog.find_patch(source)) {
        function(*patch);
        return;
    }
    if (const auto group = catalog.find_group(source)) {
        for (const auto& member : catalog.group(*group).members) {
            // Group structure admission guarantees that every member resolves to an owning patch.
            function(*catalog.find_patch(member));
        }
    }
}

[[nodiscard]] bool relationship_exists(const catalog::Catalog& catalog, std::string_view source) {
    return catalog.find_patch(source).has_value() || catalog.find_group(source).has_value();
}

[[nodiscard]] bool stable_id_less(std::string_view left, std::string_view right) {
    const auto folded_left = catalog::fold_ascii(left);
    const auto folded_right = catalog::fold_ascii(right);
    return folded_left != folded_right ? folded_left < folded_right : left < right;
}

// Selected definitions contribute common relationships followed by the selected support's appended relationships.
template <class Function>
void for_each_relationship(const catalog::PatchDefinitionRecord& patch, const catalog::SupportDefinitionRecord& support,
                           bool required, Function&& function) {
    const auto& common = required ? patch.depends_on : patch.includes;
    const auto& specialized = required ? support.depends_on : support.includes;
    for (const auto& source : common) {
        function(source);
    }
    for (const auto& source : specialized) {
        function(source);
    }
}

void close_selection(const ResolutionInput& input, PatchWorkSet& work, std::vector<SelectionDecision>& decisions) {
    // Processing repeats only when an unfixed provider is newly selected, so the closure is monotonic and finite.
    bool advanced = true;
    while (advanced) {
        advanced = false;
        for (std::size_t index = 0; index < decisions.size(); ++index) {
            auto& decision = decisions[index];
            if (!decision.applicable || !decision.selected || decision.processed) {
                continue;
            }
            decision.processed = true;
            const auto patch_index = catalog::PatchIndex{static_cast<std::uint32_t>(index)};
            const auto& patch = input.catalog.patch(patch_index);
            const auto& support = selected_support(input.catalog, patch_index);
            auto& record = work.record(patch_index);

            const auto select_target = [&](catalog::PatchIndex provider) {
                auto& provider_decision = decisions[provider.value];
                if (provider_decision.applicable && !provider_decision.fixed && !provider_decision.selected &&
                    work.record(provider).state == PatchState::Pending) {
                    provider_decision.selected = true;
                    advanced = true;
                }
            };

            // Hard relationships both select and lower an availability edge; soft includes perform selection only.
            for_each_relationship(patch, support, true, [&](std::string_view source) {
                if (!relationship_exists(input.catalog, source)) {
                    if (record.state == PatchState::Pending) {
                        // An unresolved requirement makes this patch unavailable; malformed declarations fail earlier.
                        finish_inactive_patch(record, PatchState::Skipped,
                                              FailureReason{.message = "Required relationship target '" +
                                                                       std::string{source} + "' does not exist",
                                                            .phase = PatchPhase::Selection,
                                                            .operation = "Resolve required relationship"});
                    }
                    return;
                }
                for_each_relationship_target(input.catalog, source, [&](catalog::PatchIndex provider) {
                    const auto duplicate = std::ranges::find_if(record.required_edges, [&](const RequiredEdge& edge) {
                        return edge.provider == provider;
                    });
                    if (duplicate == record.required_edges.end()) {
                        record.required_edges.push_back({provider, source});
                    } else if (stable_id_less(source, duplicate->declared_source)) {
                        // A duplicate provider retains the stable lowest source ID for later group/member diagnostics.
                        duplicate->declared_source = source;
                    }
                    select_target(provider);
                });
            });
            for_each_relationship(patch, support, false, [&](std::string_view source) {
                if (!relationship_exists(input.catalog, source)) {
                    return;
                }
                for_each_relationship_target(input.catalog, source, select_target);
            });
        }
    }

    for (std::size_t index = 0; index < decisions.size(); ++index) {
        auto& record = work.record(catalog::PatchIndex{static_cast<std::uint32_t>(index)});
        if (record.state == PatchState::Pending && decisions[index].applicable && !decisions[index].selected) {
            record.state = PatchState::Disabled;
        }
    }
}

// Tarjan components mark every participant simultaneously, including one node with an edge back to itself.
void fail_required_cycles(PatchWorkSet& work) {
    const auto count = work.records().size();
    std::vector<int> discovery(count, -1);
    std::vector<int> low(count, -1);
    std::vector<catalog::PatchIndex> stack;
    std::vector<bool> on_stack(count);
    int next{};

    std::function<void(catalog::PatchIndex)> visit = [&](catalog::PatchIndex patch) {
        // Assign the node's discovery index, then fold each active required edge into its reachable low link.
        discovery[patch.value] = next;
        low[patch.value] = next++;
        stack.push_back(patch);
        on_stack[patch.value] = true;
        for (const auto& edge : work.record(patch).required_edges) {
            // A terminal provider cannot participate in a cycle among still-viable selected patches.
            if (work.record(edge.provider).state != PatchState::Pending) {
                continue;
            }
            if (discovery[edge.provider.value] == -1) {
                visit(edge.provider);
                low[patch.value] = std::min(low[patch.value], low[edge.provider.value]);
            } else if (on_stack[edge.provider.value]) {
                low[patch.value] = std::min(low[patch.value], discovery[edge.provider.value]);
            }
        }
        // A root owns a complete strongly connected component only when its low-link equals its discovery index.
        if (low[patch.value] != discovery[patch.value]) {
            return;
        }
        // Pop the component atomically so every member receives the same cycle decision from the frozen graph.
        std::vector<catalog::PatchIndex> component;
        while (!stack.empty()) {
            const auto member = stack.back();
            stack.pop_back();
            on_stack[member.value] = false;
            component.push_back(member);
            if (member == patch) {
                break;
            }
        }
        // A singleton is a cycle only when it explicitly requires itself; larger components are always cyclic.
        const bool self_cycle =
            component.size() == 1 &&
            std::ranges::any_of(work.record(component.front()).required_edges, [&](const RequiredEdge& edge) {
                return edge.provider == component.front();
            });
        if (component.size() == 1 && !self_cycle) {
            return;
        }
        for (const auto member : component) {
            auto& record = work.record(member);
            if (record.state == PatchState::Pending) {
                finish_inactive_patch(record, PatchState::Failed,
                                      FailureReason{.message = "Patch participates in a cycle of required edges",
                                                    .phase = PatchPhase::Selection,
                                                    .operation = "Validate required relationships"});
            }
        }
    };

    // Start a traversal at each surviving root not already reached through another patch's required edges.
    for (std::size_t index = 0; index < count; ++index) {
        const auto patch = catalog::PatchIndex{static_cast<std::uint32_t>(index)};
        if (work.record(patch).state == PatchState::Pending && discovery[index] == -1) {
            visit(patch);
        }
    }
}

void resolve_settings(const ResolutionInput& input, PatchWorkSet& work) {
    // Resolve settings only for selected survivors; terminal patches never construct values exposed to plugins.
    for (auto& record : work.records()) {
        if (record.state != PatchState::Pending) {
            continue;
        }
        const auto& patch = input.catalog.patch(record.patch);
        const auto& support = selected_support(input.catalog, record.patch);
        const auto schema = effective_settings(patch, support);
        const auto plugin = input.catalog.patch_plugin(record.patch);
        const auto ordinal = input.catalog.patch_ordinal(record.patch);
        const auto& raw = plugin_configuration(input, plugin).patches[ordinal].settings;

        ResolvedSettings settings;
        if (raw.size() != schema.size()) {
            finish_inactive_patch(
                record, PatchState::Failed,
                FailureReason{.message = "Configuration snapshot is not aligned with the effective schema",
                              .phase = PatchPhase::Settings,
                              .operation = "Resolve settings"});
            continue;
        }
        // Choose the authoritative source before conversion so malformed lower-precedence text is never consulted.
        for (std::size_t index = 0; index < schema.size(); ++index) {
            if (raw[index].environment_error) {
                finish_inactive_patch(record, PatchState::Failed,
                                      FailureReason{.message = *raw[index].environment_error,
                                                    .phase = PatchPhase::Settings,
                                                    .operation = "Read setting environment override"});
                break;
            }
            std::string default_text;
            std::string_view source;
            if (raw[index].environment) {
                source = *raw[index].environment;
            } else if (raw[index].ini) {
                source = raw[index].ini->value;
            } else {
                default_text = config::format_setting_default(schema[index]);
                source = default_text;
            }
            auto value = config::resolve_setting_value(schema[index], source);
            if (!value) {
                finish_inactive_patch(record, PatchState::Failed,
                                      FailureReason{.message = "Setting '" + schema[index].key + "' " + value.error(),
                                                    .phase = PatchPhase::Settings,
                                                    .operation = "Convert setting value"});
                break;
            }
            settings.push(std::move(*value));
        }
        if (record.state == PatchState::Pending) {
            // Publish values only after the entire effective schema converts, avoiding partial callback settings.
            record.settings = std::move(settings);
        }
    }
}

void mark_waiting_images(const ResolutionInput& input, PatchWorkSet& work) {
    // A selected patch can remain viable while its supported DLL is not yet present in the recognized target.
    for (auto& record : work.records()) {
        if (record.state != PatchState::Pending) {
            continue;
        }
        const auto image = selected_support(input.catalog, record.patch).image;
        if (input.target.find(image) == nullptr) {
            record.state = PatchState::WaitingForImage;
        }
    }
}

[[nodiscard]] bool provider_available(PatchState state) noexcept {
    return state == PatchState::Pending || state == PatchState::Ready || state == PatchState::Installed;
}

[[nodiscard]] bool unavailable_edge_less(const PatchWorkSet& patches, const RequiredEdge& left,
                                         const RequiredEdge& right) {
    const auto left_source = catalog::fold_ascii(left.declared_source);
    const auto right_source = catalog::fold_ascii(right.declared_source);
    if (left_source != right_source) {
        return left_source < right_source;
    }
    return stable_id_less(patches.catalog().patch(left.provider).id, patches.catalog().patch(right.provider).id);
}

} // namespace

PatchWorkSet resolve_patches(const ResolutionInput& input) {
    PatchWorkSet work{input.catalog};
    std::vector<SelectionDecision> decisions(input.catalog.patch_count());
    // Freeze user-controlled choices, then expand required and included relationships to a stable selection closure.
    establish_fixed_decisions(input, work, decisions);
    close_selection(input, work, decisions);

    // Relationship failures propagate before settings so unavailable consumers never perform irrelevant conversion.
    fail_required_cycles(work);
    prune_unavailable_consumers(work);
    resolve_settings(input, work);
    prune_unavailable_consumers(work);

    // Image readiness is the final nonterminal classification and can itself make required consumers wait or skip.
    mark_waiting_images(input, work);
    prune_unavailable_consumers(work);

    // Emit the settled selection outcome once; intermediate closure passes would otherwise produce misleading noise.
    std::size_t selected_count{};
    std::size_t waiting_count{};
    for (const auto& record : work.records()) {
        const auto& definition = input.catalog.patch(record.patch);
        if (record.state == PatchState::Pending) {
            ++selected_count;
            input.logger.debug("Selected patch '{}' for validation", definition.id);
        } else if (record.state == PatchState::WaitingForImage) {
            ++selected_count;
            ++waiting_count;
            input.logger.info("Patch '{}' is waiting for its reviewed target image", definition.id);
        } else if (record.state == PatchState::Skipped && record.reason) {
            input.logger.warning("Patch '{}' was skipped during selection: {}", definition.id, record.reason->message);
        } else if (record.state == PatchState::Failed && record.reason) {
            input.logger.error("Patch '{}' failed during selection: {}", definition.id, record.reason->message);
        } else if (record.state == PatchState::Disabled) {
            input.logger.debug("Patch '{}' is disabled by configuration", definition.id);
        } else if (record.state == PatchState::NotApplicable) {
            input.logger.debug("Patch '{}' does not apply to the recognized target", definition.id);
        }
    }
    input.logger.info("Selected {} patch(es), including {} waiting for a late image", selected_count, waiting_count);
    return work;
}

void prune_unavailable_consumers(PatchWorkSet& patches) {
    // Each pass terminalizes at least one consumer; retained edges and selection are never rebuilt or backtracked.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& consumer : patches.records()) {
            if (consumer.state != PatchState::Pending && consumer.state != PatchState::Ready &&
                consumer.state != PatchState::WaitingForImage) {
                continue;
            }
            const RequiredEdge* unavailable{};
            for (const auto& edge : consumer.required_edges) {
                if (!provider_available(patches.record(edge.provider).state) &&
                    (unavailable == nullptr || unavailable_edge_less(patches, edge, *unavailable))) {
                    unavailable = &edge;
                }
            }
            if (unavailable == nullptr) {
                continue;
            }
            // The immediate edge is deterministic, while its phase follows the first root failure through the chain.
            const auto& provider_record = patches.record(unavailable->provider);
            const auto& provider = patches.catalog().patch(unavailable->provider);
            const auto group = patches.catalog().find_group(unavailable->declared_source);
            const auto root_phase = provider_record.reason && provider_record.reason->phase
                                        ? *provider_record.reason->phase
                                        : PatchPhase::Selection;
            finish_inactive_patch(
                consumer, PatchState::Skipped,
                FailureReason{.message = "Required patch is unavailable",
                              .phase = root_phase,
                              .operation = "Prune unavailable requirement",
                              .related_patch = provider.id,
                              .related_group = group
                                                   ? std::optional<std::string_view>{patches.catalog().group(*group).id}
                                                   : std::nullopt});
            changed = true;
        }
    }
}

} // namespace fc::planning
