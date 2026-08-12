#include "catalog.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <expected>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fusioncutter::catalog {
namespace {

[[nodiscard]] bool same_variant_key(const PatchVariant& left, const PatchVariant& right) noexcept {
    return left.layout == right.layout && left.role == right.role && left.image == right.image;
}

[[nodiscard]] bool applies_to_scope(const PatchVariant& variant, CatalogScope scope) noexcept {
    return target_architecture(variant.layout) == scope.architecture && scope.includes(variant.role);
}

[[nodiscard]] bool applies_to_environment(const PatchVariant& variant, const TargetContext& target) noexcept {
    return variant.layout == target.layout && variant.role == target.role &&
           target_architecture(variant.layout) == target.image.architecture;
}

[[nodiscard]] OutcomeReason catalog_error(std::string message, PatchId patch_id = {}) {
    return {std::move(message), std::nullopt,
            patch_id.empty() ? std::optional<PatchId>{} : std::optional<PatchId>{patch_id}};
}

[[nodiscard]] const PatchVariant* applicable_variant(const CatalogEntry& entry, const TargetContext& target) noexcept {
    const auto match = std::ranges::find_if(entry.definition.variants, [&](const auto& variant) {
        return applies_to_environment(variant, target);
    });
    return match == entry.definition.variants.end() ? nullptr : &*match;
}

[[nodiscard]] std::optional<bool> explicit_override(PatchId id, std::span<const PatchOverride> overrides) noexcept {
    std::optional<bool> result;
    for (const auto& override_value : overrides) {
        if (override_value.patch_id == id) {
            result = override_value.enabled;
        }
    }
    return result;
}

[[nodiscard]] std::size_t patch_index(std::span<const CatalogEntry> entries, PatchId id) noexcept {
    const auto found = std::ranges::lower_bound(entries, id, {}, &CatalogEntry::id);
    assert(found != entries.end() && found->id == id);
    return static_cast<std::size_t>(found - entries.begin());
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_references(const Catalog& catalog) {
    for (const auto& entry : catalog.entries()) {
        for (const auto dependency : entry.definition.depends_on) {
            if (catalog.find(dependency) == nullptr) {
                return std::unexpected(
                    catalog_error("required patch '" + std::string(dependency) + "' does not exist", entry.id));
            }
        }
        for (const auto selected : entry.definition.includes) {
            if (catalog.find(selected) == nullptr) {
                return std::unexpected(catalog_error(
                    "patch '" + std::string(selected) + "' referenced by includes does not exist", entry.id));
            }
        }
    }
    return {};
}

[[nodiscard]] std::expected<std::vector<std::size_t>, std::size_t>
dependency_order(std::span<const CatalogEntry> entries, std::span<const std::size_t> included_indices) {
    std::vector<bool> included(entries.size());
    std::vector<std::size_t> remaining_dependencies(entries.size());
    for (const auto index : included_indices) {
        included[index] = true;
    }
    for (const auto index : included_indices) {
        for (const auto dependency : entries[index].definition.depends_on) {
            if (included[patch_index(entries, dependency)]) {
                ++remaining_dependencies[index];
            }
        }
    }

    std::vector<bool> emitted(entries.size());
    std::vector<std::size_t> order;
    order.reserve(included_indices.size());
    while (order.size() != included_indices.size()) {
        std::size_t next = entries.size();
        for (const auto index : included_indices) {
            if (!emitted[index] && remaining_dependencies[index] == 0) {
                next = index;
                break;
            }
        }
        if (next == entries.size()) {
            for (const auto index : included_indices) {
                if (!emitted[index]) {
                    return std::unexpected(index);
                }
            }
        }

        emitted[next] = true;
        order.push_back(next);
        for (const auto index : included_indices) {
            if (emitted[index]) {
                continue;
            }
            for (const auto dependency : entries[index].definition.depends_on) {
                if (dependency == entries[next].id) {
                    --remaining_dependencies[index];
                }
            }
        }
    }
    return order;
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_required_graph(const Catalog& catalog) {
    const auto entries = catalog.entries();
    std::vector<std::size_t> all_indices(entries.size());
    std::iota(all_indices.begin(), all_indices.end(), 0);

    const auto order = dependency_order(entries, all_indices);
    if (!order.has_value()) {
        return std::unexpected(catalog_error("required patch dependency cycle detected", entries[order.error()].id));
    }
    return {};
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_presentation(const CatalogEntry& entry) {
    if (entry.definition.name.empty() || entry.definition.category.name.empty()) {
        return std::unexpected(catalog_error("patch presentation metadata is incomplete", entry.id));
    }
    if (!entry.definition.configurable && !entry.definition.settings.metadata().empty()) {
        return std::unexpected(catalog_error("nonconfigurable patch declares user settings", entry.id));
    }
    if (auto settings_result = entry.definition.settings.validate_metadata(); !settings_result.has_value()) {
        auto reason = std::move(settings_result.error());
        reason.related_patch = entry.id;
        return std::unexpected(std::move(reason));
    }
    return {};
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_variants(const CatalogEntry& entry, CatalogScope scope) {
    bool applies_to_artifact = false;
    for (std::size_t index = 0; index < entry.definition.variants.size(); ++index) {
        const auto& variant = entry.definition.variants[index];
        const auto& settings = variant_settings(entry.definition, variant);
        if (variant.factory.construct == nullptr || variant.factory.settings_type != settings.settings_type()) {
            return std::unexpected(
                catalog_error("variant factory is missing or uses the wrong settings type", entry.id));
        }
        if (!entry.definition.configurable && !settings.metadata().empty()) {
            return std::unexpected(catalog_error("nonconfigurable patch declares user settings", entry.id));
        }
        if (variant.settings_override.has_value()) {
            if (auto settings_result = settings.validate_metadata(); !settings_result.has_value()) {
                auto reason = std::move(settings_result.error());
                reason.related_patch = entry.id;
                return std::unexpected(std::move(reason));
            }
        }
        if (!entry.build_envelope.supports(variant.role) ||
            !entry.build_envelope.supports(target_architecture(variant.layout))) {
            return std::unexpected(catalog_error("variant exceeds its manifest build envelope", entry.id));
        }
        if (variant.image_timing == ImageTiming::OneShotLate &&
            variant.failure_policy == StartupFailurePolicy::StartupRequired) {
            return std::unexpected(catalog_error("one-shot late-image variant cannot be startup-required", entry.id));
        }
        applies_to_artifact = applies_to_artifact || applies_to_scope(variant, scope);

        const auto remaining = entry.definition.variants.subspan(index + 1);
        if (std::ranges::any_of(remaining, [&](const auto& other) {
                return same_variant_key(variant, other);
            })) {
            return std::unexpected(catalog_error("multiple variants use the same target tuple", entry.id));
        }
        if (std::ranges::any_of(remaining, [&](const auto& other) {
                return variant.layout == other.layout && variant.role == other.role;
            })) {
            return std::unexpected(catalog_error("multiple variants apply to the same layout and role", entry.id));
        }
    }

    if (!applies_to_artifact) {
        return std::unexpected(catalog_error("patch has no variant applicable to this artifact", entry.id));
    }
    return {};
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_category_order(std::span<const CatalogEntry> entries) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        for (const auto& other : entries.subspan(index + 1)) {
            if (entry.definition.configurable && other.definition.configurable &&
                entry.definition.category.name == other.definition.category.name &&
                entry.definition.category.order != other.definition.category.order) {
                return std::unexpected(catalog_error("presentation category order is inconsistent", other.id));
            }
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_definitions(const Catalog& catalog, CatalogScope scope) {
    const auto entries = catalog.entries();
    for (const auto& entry : entries) {
        if (!entry.build_envelope.supports(scope.architecture) ||
            (!(entry.build_envelope.client && scope.client) && !(entry.build_envelope.server && scope.server))) {
            return std::unexpected(catalog_error("manifest build envelope does not include this artifact", entry.id));
        }
        if (auto result = validate_presentation(entry); !result.has_value()) {
            return result;
        }
        if (auto result = validate_variants(entry, scope); !result.has_value()) {
            return result;
        }
    }
    return validate_category_order(entries);
}

} // namespace

const CatalogEntry* Catalog::find(PatchId id) const noexcept {
    const auto found = std::ranges::lower_bound(entries_, id, {}, &CatalogEntry::id);
    return found != entries_.end() && found->id == id ? &*found : nullptr;
}

CatalogEntry catalog_entry(PatchId id, PatchDefinition definition, PatchBuildEnvelope build_envelope) noexcept {
    return {id, std::move(definition), build_envelope};
}

std::expected<Catalog, OutcomeReason> initialize_catalog(std::vector<CatalogEntry> entries, CatalogScope scope) {
    std::ranges::sort(entries, {}, &CatalogEntry::id);
    const auto duplicate = std::ranges::adjacent_find(entries, {}, &CatalogEntry::id);
    if (duplicate != entries.end()) {
        return std::unexpected(catalog_error("duplicate patch ID", duplicate->id));
    }

    Catalog catalog(std::move(entries));
    if (auto result = validate_references(catalog); !result.has_value()) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = validate_required_graph(catalog); !result.has_value()) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = validate_definitions(catalog, scope); !result.has_value()) {
        return std::unexpected(std::move(result.error()));
    }
    return catalog;
}

std::vector<config::ApplicablePatch> configurable_patches(const Catalog& catalog, const TargetContext& target) {
    std::vector<config::ApplicablePatch> result;
    for (const auto& entry : catalog.entries()) {
        const auto* variant = applicable_variant(entry, target);
        if (entry.definition.configurable && variant != nullptr) {
            result.push_back({entry.id, &entry.definition, &variant_settings(entry.definition, *variant)});
        }
    }
    return result;
}

PatchSelection select_patches(const Catalog& catalog, const TargetContext& target,
                              std::span<const PatchOverride> overrides) {
    struct SelectionState {
        const PatchVariant* variant{};
        bool explicitly_disabled{};
        bool selected{};
    };

    const auto entries = catalog.entries();
    std::vector<SelectionState> states(entries.size());
    std::vector<std::size_t> pending;

    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto& state = states[index];
        state.variant = applicable_variant(entries[index], target);
        const auto override_value = explicit_override(entries[index].id, overrides);
        state.explicitly_disabled = override_value.has_value() && !*override_value;
        const auto enabled = override_value.value_or(entries[index].definition.enabled);
        state.selected = state.variant != nullptr && enabled;
        if (state.selected) {
            pending.push_back(index);
        }
    }

    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto patch = pending[cursor];
        const auto select_related = [&](PatchId id) {
            const auto related_index = patch_index(entries, id);
            auto& related = states[related_index];
            if (!related.selected && !related.explicitly_disabled && related.variant != nullptr) {
                related.selected = true;
                pending.push_back(related_index);
            }
        };
        for (const auto dependency : entries[patch].definition.depends_on) {
            select_related(dependency);
        }
        for (const auto related : entries[patch].definition.includes) {
            select_related(related);
        }
    }

    PatchSelection result;
    std::vector<std::size_t> selected_indices;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& state = states[index];
        if (state.selected) {
            selected_indices.push_back(index);
        } else if (state.explicitly_disabled || state.variant != nullptr) {
            result.disabled.push_back(&entries[index]);
        } else {
            result.not_applicable.push_back(&entries[index]);
        }
    }

    const auto order = dependency_order(entries, selected_indices);
    assert(order.has_value());
    for (const auto index : *order) {
        result.install_order.push_back({&entries[index], states[index].variant});
    }
    return result;
}

} // namespace fusioncutter::catalog
