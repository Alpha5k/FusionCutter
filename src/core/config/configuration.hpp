#pragma once

#include "configuration_types.hpp"

#include <expected>
#include <string>

namespace fc::config {

// One successful load returns raw plugin values plus the framework options derived in the same pass.
struct ConfigurationLoadResult {
    PluginConfiguration configuration;
    std::optional<FrameworkOptions> framework;
};

// Loads or creates exactly one survivor file. A returned error is a whole-file gate failure.
[[nodiscard]] std::expected<ConfigurationLoadResult, std::string>
load_configuration(const catalog::PluginDefinitionRecord& plugin, bool core, const ConfigurationPaths& paths);

// Exposed to focused tests so deterministic layout does not require filesystem observation.
[[nodiscard]] std::expected<std::string, std::string>
render_configuration(const catalog::PluginDefinitionRecord& plugin, bool core);

} // namespace fc::config
