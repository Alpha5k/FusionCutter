#pragma once

#include "catalog_types.hpp"
#include "plugin_discovery.hpp"

#include "../config/configuration_types.hpp"
#include "../targets/recognition.hpp"

#include <filesystem>
#include <vector>

namespace fc::catalog {

// Separates ordinary plugin rejection from fatal built-in Core plugin admission or framework configuration failure.
struct CatalogBuildResult {
    std::optional<Catalog> catalog;
    std::optional<config::ConfigurationSnapshot> configuration;
    std::vector<RejectionRecord> rejections;
    std::optional<std::string> fatal_error;
};

// The lifecycle owner may observe the one internal transition that occurs inside the atomic build operation.
struct CatalogBuildObserver {
    void* context{};
    void (*begin_configuration)(void* context) noexcept {};
};

// Owns the complete forward-only acquisition, admission, target selection, configuration, and transfer operation.
class CatalogBuilder {
  public:
    // The host table and its callbacks must remain valid through the one permitted build operation.
    CatalogBuilder(const FC_HostApi& host, CodeOwner core_code_owner);

    // Contributions retain their registration state only if the common admission path accepts them.
    void add_core(RegistrationBridge contribution);
    void add_bundled(RegistrationBridge contribution);
    // Testing and verifier hosts provide an already normalized, inspected set instead of scanning installation files.
    void set_external_discovery(DiscoveryResult discovery);

    // One call owns acquisition through configuration and transfers only complete survivors to the plugin catalog.
    [[nodiscard]] CatalogBuildResult build(const targets::RecognizedTarget& target,
                                           const config::ConfigurationPaths& paths, CatalogBuildObserver observer = {});

  private:
    const FC_HostApi* host_{};
    CodeOwner core_code_owner_;
    RegistrationBridge core_{};
    std::vector<RegistrationBridge> bundles_;
    std::optional<DiscoveryResult> external_discovery_;
    bool built_{}; // Registration bridges are consumable, so this builder is deliberately one-shot.
};

// Returns the built-in Core plugin factory through the same registration contract used by other contributions.
[[nodiscard]] RegistrationBridge core_registration_bridge() noexcept;
// Returns configured bundled plugins in declaration order; admission then orders them by stable plugin ID.
[[nodiscard]] std::span<const RegistrationBridge> configured_bundle_bridges() noexcept;

// Creates the production builder, attaches built-in contributions, and performs the one complete catalog build.
[[nodiscard]] CatalogBuildResult acquire_catalog(const FC_HostApi& host, const targets::RecognizedTarget& target,
                                                 const std::filesystem::path& installation_directory,
                                                 CatalogBuildObserver observer = {});

// Runs the production admission/configuration owner against only the caller's normalized explicit plugin paths.
[[nodiscard]] CatalogBuildResult acquire_catalog_explicit(const FC_HostApi& host,
                                                          const targets::RecognizedTarget& target,
                                                          const std::filesystem::path& installation_directory,
                                                          std::span<const std::filesystem::path> plugin_paths,
                                                          CatalogBuildObserver observer = {});

} // namespace fc::catalog
