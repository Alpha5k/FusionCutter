#include "configuration.hpp"

#include <ini.h>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fusioncutter::config {
namespace {

constexpr std::size_t kMaximumFileSize = 256 * 1024;
constexpr std::size_t kMaximumGeneratedLine = INI_MAX_LINE - 3;
// inih stores the current section in char[50], leaving 49 bytes before the terminating NUL.
constexpr std::size_t kMaximumSectionName = 49;
constexpr std::size_t kMaximumDiagnostics = 32;
constexpr std::size_t kMaximumDisplayedValue = 160;
constexpr std::uint32_t kMaximumDiagnosticsFileSizeMb = 65'535;
constexpr std::wstring_view kClientConfigurationFilename = L"FusionCutter.ini";
constexpr std::wstring_view kServerConfigurationFilename = L"FusionCutter-Server.ini";
const int kModuleAnchor{};

using settings_detail::ascii_iequals;

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] OutcomeReason file_error(const std::filesystem::path& path, std::string message) {
    return {std::move(message), "Read configuration '" + path_text(path) + "'", std::nullopt};
}

[[nodiscard]] OutcomeReason patch_error(PatchId patch_id, std::string message) {
    return {std::move(message), "Read configuration", patch_id};
}

[[nodiscard]] std::string displayed_value(std::string_view value) {
    if (value.size() <= kMaximumDisplayedValue) {
        return std::string(value);
    }
    return std::string(value.substr(0, kMaximumDisplayedValue)) + "...";
}

[[nodiscard]] bool valid_utf8(std::string_view content) noexcept {
    if (content.empty()) {
        return true;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.data(), static_cast<int>(content.size()), nullptr,
                               0) != 0;
}

[[nodiscard]] bool has_lone_carriage_return(std::string_view content) noexcept {
    for (std::size_t position = content.find('\r'); position != std::string_view::npos;
         position = content.find('\r', position + 1)) {
        if (position + 1 == content.size() || content[position + 1] != '\n') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::expected<std::optional<std::string>, OutcomeReason>
read_existing_file(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected(file_error(path, "could not inspect the configuration file: " + error.message()));
    }
    if (!exists) {
        return std::optional<std::string>{};
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::unexpected(file_error(path, "the existing configuration file could not be opened"));
    }

    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(file_error(path, "the existing configuration file size could not be read"));
    }
    if (static_cast<std::uintmax_t>(end) > kMaximumFileSize) {
        return std::unexpected(file_error(path, "the existing configuration file exceeds 256 KiB"));
    }

    std::string content(static_cast<std::size_t>(end), '\0');
    input.seekg(0);
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!input && !content.empty()) {
        return std::unexpected(file_error(path, "the existing configuration file could not be read completely"));
    }
    if (content.contains('\0')) {
        return std::unexpected(file_error(path, "the existing configuration file contains an embedded NUL byte"));
    }
    if (!valid_utf8(content)) {
        return std::unexpected(file_error(path, "the existing configuration file is not valid UTF-8"));
    }
    if (has_lone_carriage_return(content)) {
        return std::unexpected(file_error(
            path, "the existing configuration contains a carriage return that is not followed by a line feed"));
    }
    return std::optional<std::string>(std::move(content));
}

[[nodiscard]] bool valid_ini_key(std::string_view value) noexcept {
    if (value.empty() || value.front() == ' ' || value.front() == '\t' || value.back() == ' ' || value.back() == '\t') {
        return false;
    }
    if (value.front() == ';' || value.front() == '#') {
        return false;
    }
    return std::ranges::none_of(value, [](char character) {
        return character == '\0' || character == '\r' || character == '\n' || character == '=' || character == ':';
    });
}

[[nodiscard]] bool valid_ini_section(std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaximumSectionName && std::ranges::none_of(value, [](char character) {
        return character == '\0' || character == '\r' || character == '\n' || character == ']';
    });
}

[[nodiscard]] bool can_emit_comment(std::string_view value) noexcept {
    return value.size() + 2 <= kMaximumGeneratedLine && !value.contains('\0') && !value.contains('\r') &&
           !value.contains('\n');
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_patches(std::span<const ApplicablePatch> patches) {
    struct SectionOwner {
        std::string name;
        PatchId patch_id;
    };

    std::vector<SectionOwner> sections{{"Logging", {}}, {"Diagnostics", {}}, {"Patches", {}}};
    for (std::size_t index = 0; index < patches.size(); ++index) {
        const auto& patch = patches[index];
        if (patch.definition == nullptr || patch.settings == nullptr || !valid_ini_key(patch.id)) {
            return std::unexpected(patch_error(patch.id, "patch configuration metadata cannot be represented in INI"));
        }
        for (const auto& other : patches.subspan(index + 1)) {
            if (ascii_iequals(patch.id, other.id)) {
                return std::unexpected(patch_error(other.id, "patch IDs collide under case-insensitive INI matching"));
            }
        }

        bool has_base_section = false;
        std::vector<std::string_view> groups;
        for (const auto& setting : patch.settings->metadata()) {
            if (!valid_ini_key(setting.key) || setting.default_value.contains('\0') ||
                setting.default_value.contains('\r') || setting.default_value.contains('\n')) {
                return std::unexpected(
                    patch_error(patch.id, "setting configuration metadata cannot be represented in INI"));
            }

            if (setting.key.size() + setting.default_value.size() + 1 > kMaximumGeneratedLine) {
                return std::unexpected(
                    patch_error(patch.id, "a generated setting would exceed the 4096-byte line limit"));
            }

            if (setting.group.empty()) {
                has_base_section = true;
            } else if (std::ranges::none_of(groups, [&](std::string_view group) {
                           return ascii_iequals(group, setting.group);
                       })) {
                const std::string section = std::string(patch.id) + "." + setting.group;
                if (!valid_ini_section(section)) {
                    return std::unexpected(
                        patch_error(patch.id, "settings section cannot be represented by the INI parser"));
                }
                groups.push_back(setting.group);
            }
        }

        if (patch.id.size() + 2 > kMaximumGeneratedLine) {
            return std::unexpected(patch_error(patch.id, "the generated patch toggle would exceed the line limit"));
        }
        if (has_base_section) {
            if (!valid_ini_section(patch.id)) {
                return std::unexpected(
                    patch_error(patch.id, "settings section cannot be represented by the INI parser"));
            }
            sections.push_back({std::string(patch.id), patch.id});
        }
        for (const auto group : groups) {
            sections.push_back({std::string(patch.id) + "." + std::string(group), patch.id});
        }
    }

    for (std::size_t index = 0; index < sections.size(); ++index) {
        for (const auto& other : std::span(sections).subspan(index + 1)) {
            if (ascii_iequals(sections[index].name, other.name)) {
                return std::unexpected(
                    patch_error(other.patch_id, "settings sections collide under case-insensitive INI matching"));
            }
        }
    }
    return {};
}

void append_comment(std::string& output, std::string_view comment) {
    if (!comment.empty() && can_emit_comment(comment)) {
        output += "; ";
        output += comment;
        output += "\r\n";
    }
}

[[nodiscard]] std::string generate_configuration(std::span<const ApplicablePatch> patches) {
    std::vector<ApplicablePatch> sorted(patches.begin(), patches.end());
    std::ranges::sort(sorted, [](const auto& left, const auto& right) {
        const auto& left_category = left.definition->category;
        const auto& right_category = right.definition->category;
        if (left_category.order != right_category.order) {
            return left_category.order < right_category.order;
        }
        if (left_category.name != right_category.name) {
            return left_category.name < right_category.name;
        }
        return left.id < right.id;
    });

    std::string output = "[Logging]\r\nLevel=";
    output += reporting_detail::default_log_level() == LogLevel::Debug ? "Debug" : "Error";
    const auto has_diagnostics = std::ranges::any_of(sorted, [](const auto& patch) {
        return patch.definition->category.name == "Diagnostics";
    });
    if (has_diagnostics) {
        output += "\r\n\r\n[Diagnostics]\r\nMaximumFileSizeMB=512";
    }
    output += "\r\n\r\n[Patches]\r\n";
    std::string_view previous_category;
    for (const auto& patch : sorted) {
        if (patch.definition->category.name != previous_category) {
            if (!previous_category.empty()) {
                output += "\r\n";
            }
            append_comment(output, patch.definition->category.name);
            previous_category = patch.definition->category.name;
        }
        append_comment(output, patch.definition->description);
        output += patch.id;
        output += patch.definition->enabled ? "=1\r\n" : "=0\r\n";
    }

    for (const auto& patch : sorted) {
        const auto metadata = patch.settings->metadata();
        std::vector<std::string_view> groups;
        if (std::ranges::any_of(metadata, [](const auto& setting) {
                return setting.group.empty();
            })) {
            groups.emplace_back();
        }
        for (const auto& setting : metadata) {
            if (!setting.group.empty() && std::ranges::none_of(groups, [&](std::string_view group) {
                    return ascii_iequals(group, setting.group);
                })) {
                groups.push_back(setting.group);
            }
        }

        for (const auto group : groups) {
            output += "\r\n[";
            output += patch.id;
            if (!group.empty()) {
                output += '.';
                output += group;
            }
            output += "]\r\n";
            for (const auto& setting : metadata) {
                if (!ascii_iequals(setting.group, group)) {
                    continue;
                }
                append_comment(output, setting.description);
                output += setting.key;
                output += '=';
                output += setting.default_value;
                output += "\r\n";
            }
        }
    }
    return output;
}

enum class CreateResult {
    Created,
    AlreadyExists,
    Failed,
};

[[nodiscard]] CreateResult create_configuration_file(const std::filesystem::path& path, std::string_view content,
                                                     std::string& error_message) {
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::noreplace);
    if (!output) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) {
            return CreateResult::AlreadyExists;
        }
        error_message = "the missing configuration file could not be created";
        if (error) {
            error_message += ": " + error.message();
        }
        return CreateResult::Failed;
    }

    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if (output) {
        return CreateResult::Created;
    }

    error_message = "the generated configuration file could not be written completely";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return CreateResult::Failed;
}

[[nodiscard]] bool already_reported(std::span<const std::string> values, std::string_view value) {
    return std::ranges::any_of(values, [&](const auto& existing) {
        return ascii_iequals(existing, value);
    });
}

[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view value) noexcept {
    constexpr std::array values{
        std::pair{std::string_view{"Off"}, LogLevel::Off},
        std::pair{std::string_view{"Error"}, LogLevel::Error},
        std::pair{std::string_view{"Warning"}, LogLevel::Warning},
        std::pair{std::string_view{"Info"}, LogLevel::Info},
        std::pair{std::string_view{"Debug"}, LogLevel::Debug},
    };
    const auto found = std::ranges::find_if(values, [&](const auto& candidate) {
        return ascii_iequals(candidate.first, value);
    });
    return found == values.end() ? std::nullopt : std::optional{found->second};
}

} // namespace

std::expected<std::filesystem::path, OutcomeReason> configuration_path(HostRole role) {
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&kModuleAnchor), &owner) ||
        owner == nullptr) {
        return std::unexpected(
            OutcomeReason{"the Fusion Cutter module directory is unavailable", "Locate configuration", {}});
    }

    std::wstring module_path(32'768, L'\0');
    const auto path_length = GetModuleFileNameW(owner, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (path_length == 0 || path_length >= module_path.size()) {
        return std::unexpected(
            OutcomeReason{"the Fusion Cutter module path is unavailable", "Locate configuration", {}});
    }
    module_path.resize(path_length);

    auto path = std::filesystem::path{std::move(module_path)};
    path.replace_filename(role == HostRole::Client ? kClientConfigurationFilename : kServerConfigurationFilename);
    return path;
}

class ConfigurationParser {
  public:
    [[nodiscard]] static std::expected<void, OutcomeReason> parse(Configuration& configuration,
                                                                  std::string_view content) {
        ParserContext context{&configuration, {}};
        const int result = ini_parse_string_length(content.data(), content.size(), handle_ini_entry, &context);
        if (result != 0) {
            const std::string location = result > 0 ? " at line " + std::to_string(result) : std::string{};
            return std::unexpected(
                file_error(configuration.path_, "the existing configuration is malformed" + location));
        }

        for (auto& patch : configuration.patch_values_) {
            PatchToggle toggle{patch.patch.id, std::nullopt, std::nullopt};
            if (patch.toggle.has_value()) {
                toggle.override_value = settings_detail::parse_value<bool>(patch.toggle->value);
                if (!toggle.override_value.has_value()) {
                    toggle.error = patch_error(patch.patch.id, "line " + std::to_string(patch.toggle->line) +
                                                                   " [Patches]." + patch.toggle->key + "='" +
                                                                   displayed_value(patch.toggle->value) +
                                                                   "': expected 0/1, true/false, or off/on");
                    configuration.add_diagnostic(patch.toggle->line, toggle.error->message);
                }
            }
            configuration.patch_toggles_.push_back(std::move(toggle));
        }
        if (configuration.logging_level_.has_value()) {
            const auto parsed = parse_log_level(configuration.logging_level_->value);
            if (parsed.has_value()) {
                configuration.log_level_ = *parsed;
            } else {
                configuration.add_diagnostic(configuration.logging_level_->line,
                                             "line " + std::to_string(configuration.logging_level_->line) +
                                                 " [Logging].Level='" +
                                                 displayed_value(configuration.logging_level_->value) +
                                                 "': expected Off, Error, Warning, Info, or Debug");
            }
        }
        if (configuration.diagnostics_maximum_file_size_.has_value()) {
            const auto& stored = *configuration.diagnostics_maximum_file_size_;
            const auto parsed = settings_detail::parse_value<std::uint32_t>(stored.value);
            if (parsed.has_value() && *parsed >= 1 && *parsed <= kMaximumDiagnosticsFileSizeMb) {
                configuration.diagnostics_maximum_file_size_mb_ = *parsed;
            } else {
                configuration.add_diagnostic(
                    stored.line, "line " + std::to_string(stored.line) + " [Diagnostics].MaximumFileSizeMB='" +
                                     displayed_value(stored.value) + "': expected an integer from 1 through 65535");
            }
        }
        return {};
    }

  private:
    struct SectionMatch {
        Configuration::PatchValues* patch;
        std::string_view group;
    };

    struct ParserContext {
        Configuration* configuration;
        std::vector<std::string> unknown_sections;
    };

    [[nodiscard]] static Configuration::PatchValues* find_patch(Configuration& configuration,
                                                                std::string_view patch_id) {
        const auto found = std::ranges::find_if(configuration.patch_values_, [&](const auto& patch) {
            return ascii_iequals(patch.patch.id, patch_id);
        });
        return found == configuration.patch_values_.end() ? nullptr : &*found;
    }

    [[nodiscard]] static std::optional<SectionMatch> find_section(Configuration& configuration,
                                                                  std::string_view section) {
        for (auto& patch : configuration.patch_values_) {
            const auto metadata = patch.patch.settings->metadata();
            if (ascii_iequals(section, patch.patch.id) && std::ranges::any_of(metadata, [](const auto& setting) {
                    return setting.group.empty();
                })) {
                return SectionMatch{&patch, {}};
            }
            for (const auto& setting : metadata) {
                if (setting.group.empty()) {
                    continue;
                }
                const std::string full_name = std::string(patch.patch.id) + "." + setting.group;
                if (ascii_iequals(section, full_name)) {
                    return SectionMatch{&patch, setting.group};
                }
            }
        }
        return std::nullopt;
    }

    static void store_value(Configuration& configuration, std::optional<Configuration::StoredValue>& destination,
                            std::string_view section, std::string_view key, std::string_view value, std::size_t line) {
        if (destination.has_value()) {
            configuration.add_diagnostic(line, "duplicate [" + std::string(section) + "]." + std::string(key) +
                                                   "; last value wins");
        }
        destination = Configuration::StoredValue{std::string(section), std::string(key), std::string(value), line};
    }

    static void report_unknown_section(ParserContext& context, std::string_view section, std::size_t line) {
        if (!already_reported(context.unknown_sections, section)) {
            context.configuration->add_diagnostic(line, "unknown configuration section [" + std::string(section) + "]");
            context.unknown_sections.emplace_back(section);
        }
    }

    static int handle_ini_entry(void* user, const char* section_text, const char* key_text, const char* value_text,
                                int line) {
        auto& context = *static_cast<ParserContext*>(user);
        auto& configuration = *context.configuration;
        const std::string_view section = section_text == nullptr ? std::string_view{} : section_text;
        const auto line_number = static_cast<std::size_t>(line);

        if (key_text == nullptr) {
            if (!ascii_iequals(section, "Logging") && !ascii_iequals(section, "Patches") &&
                !(configuration.has_diagnostics_ && ascii_iequals(section, "Diagnostics")) &&
                !find_section(configuration, section).has_value()) {
                report_unknown_section(context, section, line_number);
            }
            return 1;
        }

        const std::string_view key = key_text;
        const std::string_view value = value_text == nullptr ? std::string_view{} : value_text;
        if (ascii_iequals(section, "Logging")) {
            if (ascii_iequals(key, "Level")) {
                store_value(configuration, configuration.logging_level_, section, key, value, line_number);
            } else {
                configuration.add_diagnostic(line_number, "unknown setting [Logging]." + std::string(key));
            }
            return 1;
        }
        if (configuration.has_diagnostics_ && ascii_iequals(section, "Diagnostics")) {
            if (ascii_iequals(key, "MaximumFileSizeMB")) {
                store_value(configuration, configuration.diagnostics_maximum_file_size_, section, key, value,
                            line_number);
            } else {
                configuration.add_diagnostic(line_number, "unknown setting [Diagnostics]." + std::string(key));
            }
            return 1;
        }
        if (ascii_iequals(section, "Patches")) {
            auto* patch = find_patch(configuration, key);
            if (patch == nullptr) {
                configuration.add_diagnostic(line_number, "unknown or unavailable patch [Patches]." + std::string(key));
            } else {
                store_value(configuration, patch->toggle, section, key, value, line_number);
            }
            return 1;
        }

        const auto matched = find_section(configuration, section);
        if (!matched.has_value()) {
            report_unknown_section(context, section, line_number);
            return 1;
        }

        const auto setting_index = matched->patch->patch.settings->find(matched->group, key);
        if (!setting_index.has_value()) {
            configuration.add_diagnostic(line_number,
                                         "unknown setting [" + std::string(section) + "]." + std::string(key));
            return 1;
        }
        store_value(configuration, matched->patch->settings[*setting_index], section, key, value, line_number);
        return 1;
    }
};

void Configuration::add_diagnostic(std::size_t line, std::string message) {
    if (diagnostics_.size() == kMaximumDiagnostics) {
        ++omitted_diagnostics_;
        return;
    }
    diagnostics_.push_back({line, std::move(message)});
}

void Configuration::use_compiled_toggles() {
    for (const auto& patch : patch_values_) {
        patch_toggles_.push_back({patch.patch.id, std::nullopt, std::nullopt});
    }
}

std::expected<ResolvedSettings, OutcomeReason> Configuration::resolve_settings(PatchId patch_id) const {
    const auto found = std::ranges::find_if(patch_values_, [&](const auto& patch) {
        return patch.patch.id == patch_id;
    });
    if (found == patch_values_.end()) {
        return std::unexpected(patch_error(patch_id, "selected patch has no applicable configuration metadata"));
    }

    auto resolved = found->patch.settings->make_defaults();
    for (std::size_t index = 0; index < found->settings.size(); ++index) {
        if (!found->settings[index].has_value()) {
            continue;
        }
        const auto& stored = *found->settings[index];
        if (auto result = found->patch.settings->apply(resolved, index, stored.value); !result.has_value()) {
            auto reason = std::move(result.error());
            reason.message = "line " + std::to_string(stored.line) + " [" + stored.section + "]." + stored.key + "='" +
                             displayed_value(stored.value) + "': " + reason.message;
            reason.operation = "Resolve configuration";
            reason.related_patch = patch_id;
            return std::unexpected(std::move(reason));
        }
    }

    if (auto result = found->patch.settings->validate(resolved); !result.has_value()) {
        auto reason = std::move(result.error());
        if (!reason.operation.has_value()) {
            reason.operation = "Validate configuration";
        }
        reason.related_patch = patch_id;
        return std::unexpected(std::move(reason));
    }
    return resolved;
}

std::expected<Configuration, OutcomeReason> load_configuration(const std::filesystem::path& path,
                                                               std::span<const ApplicablePatch> patches) {
    if (auto validation = validate_patches(patches); !validation.has_value()) {
        return std::unexpected(std::move(validation.error()));
    }

    Configuration configuration;
    configuration.path_ = path;
    configuration.patch_values_.reserve(patches.size());
    configuration.patch_toggles_.reserve(patches.size());
    for (const auto& patch : patches) {
        configuration.has_diagnostics_ |= patch.definition->category.name == "Diagnostics";
        configuration.patch_values_.push_back(
            {patch, std::nullopt,
             std::vector<std::optional<Configuration::StoredValue>>(patch.settings->metadata().size())});
    }

    auto content = read_existing_file(path);
    if (!content.has_value()) {
        return std::unexpected(std::move(content.error()));
    }
    if (content->has_value()) {
        if (auto parsed = ConfigurationParser::parse(configuration, **content); !parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        return configuration;
    }

    const auto generated = generate_configuration(patches);
    if (generated.size() > kMaximumFileSize) {
        return std::unexpected(OutcomeReason{"the generated configuration would exceed 256 KiB",
                                             "Generate configuration '" + path_text(path) + "'", std::nullopt});
    }
    std::string write_error;
    switch (create_configuration_file(path, generated, write_error)) {
    case CreateResult::Created:
        configuration.file_created_ = true;
        configuration.use_compiled_toggles();
        return configuration;
    case CreateResult::AlreadyExists: {
        auto raced_content = read_existing_file(path);
        if (!raced_content.has_value()) {
            return std::unexpected(std::move(raced_content.error()));
        }
        if (!raced_content->has_value()) {
            configuration.output_error_ = file_error(path, "the configuration file disappeared during creation");
            configuration.use_compiled_toggles();
            return configuration;
        }
        if (auto parsed = ConfigurationParser::parse(configuration, **raced_content); !parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        return configuration;
    }
    case CreateResult::Failed:
        configuration.output_error_ = {std::move(write_error), "Write configuration '" + path_text(path) + "'",
                                       std::nullopt};
        configuration.use_compiled_toggles();
        return configuration;
    }
    std::unreachable();
}

} // namespace fusioncutter::config
