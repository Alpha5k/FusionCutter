#pragma once

#include <FusionCutter/patch.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fusioncutter::config {

class ConfigurationParser;

struct ApplicablePatch {
    PatchId id;
    const PatchDefinition* definition;
};

struct ConfigurationDiagnostic {
    std::size_t line;
    std::string message;
};

struct PatchToggle {
    PatchId patch_id;
    std::optional<bool> override_value;
    std::optional<OutcomeReason> error;
};

class Configuration {
  public:
    [[nodiscard]] std::span<const PatchToggle> patch_toggles() const noexcept {
        return patch_toggles_;
    }

    [[nodiscard]] std::span<const ConfigurationDiagnostic> diagnostics() const noexcept {
        return diagnostics_;
    }

    [[nodiscard]] std::size_t omitted_diagnostics() const noexcept {
        return omitted_diagnostics_;
    }

    [[nodiscard]] bool file_created() const noexcept {
        return file_created_;
    }

    [[nodiscard]] const std::optional<OutcomeReason>& output_error() const noexcept {
        return output_error_;
    }

    [[nodiscard]] LogLevel log_level() const noexcept {
        return log_level_;
    }

    // Call only for a patch selected by the catalog. Disabled patches intentionally skip typed setting validation.
    [[nodiscard]] std::expected<ResolvedSettings, OutcomeReason> resolve_settings(PatchId patch_id) const;

  private:
    struct StoredValue {
        std::string section;
        std::string key;
        std::string value;
        std::size_t line;
    };

    struct PatchValues {
        ApplicablePatch patch;
        std::optional<StoredValue> toggle;
        std::vector<std::optional<StoredValue>> settings;
    };

    friend std::expected<Configuration, OutcomeReason> load_configuration(const std::filesystem::path& path,
                                                                          std::span<const ApplicablePatch> patches);
    friend class ConfigurationParser;

    void add_diagnostic(std::size_t line, std::string message);
    void use_compiled_toggles();

    std::filesystem::path path_;
    std::vector<PatchValues> patch_values_;
    std::vector<PatchToggle> patch_toggles_;
    std::vector<ConfigurationDiagnostic> diagnostics_;
    std::optional<StoredValue> logging_level_;
    std::size_t omitted_diagnostics_{};
    bool file_created_{};
    std::optional<OutcomeReason> output_error_;
    LogLevel log_level_{compiled_default_log_level()};
};

[[nodiscard]] std::expected<Configuration, OutcomeReason> load_configuration(const std::filesystem::path& path,
                                                                             std::span<const ApplicablePatch> patches);

[[nodiscard]] std::expected<std::filesystem::path, OutcomeReason> configuration_path(HostRole role);

} // namespace fusioncutter::config
