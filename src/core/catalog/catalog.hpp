#pragma once

#include "../config/configuration.hpp"

#include <FusionCutter/patch.hpp>

#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace fusioncutter::catalog {

struct CatalogEntry {
    PatchId id;
    PatchDefinition definition;
    PatchBuildEnvelope build_envelope;
};

struct CatalogScope {
    Architecture architecture;
    bool client;
    bool server;

    [[nodiscard]] constexpr bool includes(HostRole role) const noexcept {
        return role == HostRole::Client ? client : server;
    }
};

class Catalog {
  public:
    explicit Catalog(std::vector<CatalogEntry> entries) noexcept : entries_(std::move(entries)) {}

    [[nodiscard]] std::span<const CatalogEntry> entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] const CatalogEntry* find(PatchId id) const noexcept;

  private:
    std::vector<CatalogEntry> entries_;
};

struct PatchOverride {
    PatchId patch_id;
    bool enabled;
};

struct SelectedPatch {
    const CatalogEntry* entry;
    const PatchVariant* variant;
};

struct PatchSelection {
    std::vector<SelectedPatch> install_order;
    std::vector<const CatalogEntry*> disabled;
    std::vector<const CatalogEntry*> not_applicable;
};

[[nodiscard]] CatalogEntry catalog_entry(PatchId id, PatchDefinition definition,
                                         PatchBuildEnvelope build_envelope) noexcept;

[[nodiscard]] std::expected<Catalog, OutcomeReason> initialize_catalog(std::vector<CatalogEntry> entries,
                                                                       CatalogScope scope);

[[nodiscard]] std::vector<config::ApplicablePatch> configurable_patches(const Catalog& catalog,
                                                                        const TargetContext& target);

[[nodiscard]] PatchSelection select_patches(const Catalog& catalog, const TargetContext& target,
                                            std::span<const PatchOverride> overrides = {});

} // namespace fusioncutter::catalog
