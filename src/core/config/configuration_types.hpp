#pragma once

#include "../catalog/catalog_types.hpp"

#include <FusionCutter/PluginApi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fc::config {

// The configuration file budget bounds both parsed input and generated output.
inline constexpr std::size_t kConfigurationFileByteCapacity = 256U * 1024U;

// A zero line denotes a diagnostic not attributable to one physical input line.
struct ConfigurationDiagnostic {
    std::size_t line{};
    std::string message;
};

// Preserves the original scalar spelling and source line until typed resolution can produce precise diagnostics.
struct StoredValue {
    std::string value;
    std::size_t line{};
};

// Environment capture stays separate so a present malformed override remains authoritative during typed resolution.
struct RawSettingValue {
    std::optional<StoredValue> ini;
    std::optional<std::string> environment;
    std::optional<std::string> environment_error;
};

// Raw patch and group values retain declaration order so resolution can index them without rebuilding identity maps.
struct PatchConfiguration {
    std::optional<StoredValue> toggle;
    std::vector<RawSettingValue> settings;
};

// A group has only its independently configurable enablement value at this stage.
struct GroupConfiguration {
    std::optional<StoredValue> toggle;
};

// Holds one plugin's raw inputs and bounded diagnostics after file and environment collection has finished.
struct PluginConfiguration {
    std::string plugin_id;
    // Both vectors use the owning final PluginDefinitionRecord's declaration order.
    std::vector<PatchConfiguration> patches;
    std::vector<GroupConfiguration> groups;
    std::vector<ConfigurationDiagnostic> diagnostics;
    std::optional<std::string> output_error;
    bool file_expected{};
    bool file_created{};
};

// Compiled defaults remain usable independently when an individual framework setting is invalid.
struct FrameworkOptions {
#if defined(NDEBUG)
    FC_LogLevel log_level{FC_LOG_ERROR};
#else
    FC_LogLevel log_level{FC_LOG_DEBUG};
#endif
    std::uint32_t max_trace_size_mb{512};
};

// This is the sole post-I/O carrier; plugins align one-for-one with the final plugin catalog's order.
struct ConfigurationSnapshot {
    FrameworkOptions framework;
    std::vector<PluginConfiguration> plugins;
};

// Derives every configuration location from one installation root without consulting process state.
struct ConfigurationPaths {
    std::filesystem::path installation_directory;

    [[nodiscard]] std::filesystem::path config_directory() const;
    [[nodiscard]] std::filesystem::path plugin_file(std::string_view plugin_id) const;
};

// Owns one typed setting value and repairs the ABI string view whenever the value is exposed.
class ResolvedSettingValue {
  public:
    // The returned union borrows string storage from this object when type() is FC_SETTING_STRING.
    [[nodiscard]] FC_SettingType type() const noexcept;
    [[nodiscard]] FC_SettingValue native_value() const noexcept;

    // Factories select the active representation; string() takes ownership of its argument.
    [[nodiscard]] static ResolvedSettingValue scalar(FC_SettingType type, FC_SettingValue value) noexcept;
    [[nodiscard]] static ResolvedSettingValue string(std::string value);

  private:
    FC_SettingType type_{};
    FC_SettingValue value_{};
    std::string string_;
};

// Converts one authoritative scalar without fallback; callers own source precedence and diagnostics.
[[nodiscard]] std::expected<ResolvedSettingValue, std::string>
resolve_setting_value(const catalog::SettingDefinitionRecord& definition, std::string_view text);

// Defaults use the same scalar spelling accepted by resolve_setting_value(), including width-specific float output.
[[nodiscard]] std::string format_setting_default(const catalog::SettingDefinitionRecord& definition);

} // namespace fc::config
