#pragma once

#include "patches/dependency.hpp"
#include "ini.hpp"
#include "plugins/plugin.hpp"

#include <filesystem>
#include <map>
#include <variant>
#include <string_view>

namespace fc::runtime {

// Retains the startup values used when this variant's handler is eventually constructed.
struct CapturedVariantSettings {
    // Borrows the immutable declaration; its plugin remains loaded while captured settings are used.
    const FC_Variant *definition{};
    // Resolved scalars in this variant's setting declaration order.
    std::vector<ScalarValue> values;
};

// Holds one plugin's resolved toggles, applicable variant values and recoverable configuration problems.
struct PluginConfiguration {
    // One result per patch, in the plugin's patch declaration order; group toggles have already been applied.
    std::vector<PatchControl> controls;
    std::vector<CapturedVariantSettings> variants;
    std::vector<Diagnostic> warnings;
};

// Selects the configuration directory and whether loading may add declaration defaults to disk.
struct ConfigurationPaths {
    // An empty directory with writes disabled skips plugin INI access during offline validation.
    std::filesystem::path directory;
    // Enables creation of nonempty missing files and insertion of missing entries into existing sections.
    bool write_defaults = true;
};

// Supplies logging and tracing limits, with warnings for individual values that fell back to defaults.
struct RuntimeSettings {
    // Minimum severity emitted by the runtime logger, or Off to disable logging.
    FC_LogLevel log_level = FC_LOG_WARNING;
    // Sequential trace file budget in bytes; zero disables trace recording.
    std::uint64_t trace_file_bytes = 512ull * 1024 * 1024;
    std::vector<Diagnostic> warnings;
};

// Returns a static configuration spelling; unrecognized levels use Warning.
std::string_view log_level_name(FC_LogLevel level) noexcept;

// Returns this runtime module's directory with default writes enabled; Windows lookup failures return a diagnostic.
std::expected<ConfigurationPaths, Diagnostic> runtime_configuration_paths();

// Loads FusionCutter.ini from paths; invalid individual values retain their defaults and add warnings.
// Read/structure failures return errors; enabled writes also return create failures and warn on existing update failures.
std::expected<RuntimeSettings, Diagnostic> load_runtime_configuration(const ConfigurationPaths &paths);

// Captures startup controls and settings for ready or waiting variants; admitted declarations must outlive the result.
// Read/parse/create errors fail loading; update errors warn. Scalar environment overrides INI; invalid values warn and use defaults.
std::expected<PluginConfiguration, Diagnostic> load_configuration(const AdmittedPlugin &plugin,
                                                                  std::span<const ResolvedPatch *const> patches, FC_Role role,
                                                                  std::span<const ImageObservation> observations,
                                                                  const ConfigurationPaths &paths);

} // namespace fc::runtime
