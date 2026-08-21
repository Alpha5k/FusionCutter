#include "configuration.hpp"

#include "../catalog/definition_copy.hpp"

#include <ini.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace fc::config {
namespace {

// Describes a patch or group toggle from the schema and its destination in the aligned configuration carrier.
struct BooleanEntry {
    std::string_view id;
    std::string_view category;
    bool default_value{};
    bool patch{};
    std::size_t index{};
};

// Resolves author spelling to the owning declaration so generated section names retain canonical case.
[[nodiscard]] std::string_view canonical_category(const catalog::PluginDefinitionRecord& plugin,
                                                  std::string_view reference) {
    if (reference.empty()) {
        return {};
    }
    const auto category = std::ranges::find_if(plugin.categories, [&](const auto& candidate) {
        return catalog::equal_ascii_case_insensitive(candidate.id, reference);
    });
    return category == plugin.categories.end() ? reference : std::string_view{category->id};
}

// Owns an INI scalar together with its original spelling and source line for schema matching and diagnostics.
struct ParsedEntry {
    std::string section;
    std::string key;
    std::string value;
    std::size_t line{};
};

// Keeps explicit sections as well as values so empty unknown sections remain diagnosable.
struct ParsedDocument {
    std::vector<ParsedEntry> entries;
    std::vector<std::pair<std::string, std::size_t>> sections;
};

// Distinguishes a successful exclusive create from a concurrent winner and an actual output failure.
enum class CreateResult {
    Created,
    AlreadyExists,
    Failed,
};

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool alphabetical(std::string_view left, std::string_view right) {
    const auto left_folded = catalog::fold_ascii(left);
    const auto right_folded = catalog::fold_ascii(right);
    return left_folded != right_folded ? left_folded < right_folded : left < right;
}

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] std::string configuration_error(const std::filesystem::path& path, std::string message) {
    return "Configuration '" + path_text(path) + "': " + std::move(message);
}

[[nodiscard]] const catalog::SupportDefinitionRecord*
selected_support(const catalog::PatchDefinitionRecord& patch) noexcept {
    return patch.selected_support && *patch.selected_support < patch.supports.size()
               ? &patch.supports[*patch.selected_support]
               : nullptr;
}

[[nodiscard]] std::span<const catalog::SettingDefinitionRecord>
effective_settings(const catalog::PatchDefinitionRecord& patch) noexcept {
    const auto* support = selected_support(patch);
    if (support == nullptr) {
        return {};
    }
    return support->has_settings == FC_TRUE ? std::span{support->settings} : std::span{patch.settings};
}

[[nodiscard]] bool applicable_group(const catalog::GroupDefinitionRecord& group,
                                    const catalog::PluginDefinitionRecord& plugin) {
    return std::ranges::any_of(group.members, [&](std::string_view member) {
        const auto patch = std::ranges::find_if(plugin.patches, [&](const auto& candidate) {
            return catalog::equal_ascii_case_insensitive(candidate.id, member);
        });
        return patch != plugin.patches.end() && patch->selected_support.has_value();
    });
}

[[nodiscard]] std::vector<BooleanEntry> boolean_entries(const catalog::PluginDefinitionRecord& plugin) {
    // One schema toggle list drives file participation, presentation, parsing, defaults, and selection input.
    std::vector<BooleanEntry> result;
    for (std::size_t index = 0; index < plugin.patches.size(); ++index) {
        const auto& patch = plugin.patches[index];
        if (patch.selected_support && patch.configurable == FC_TRUE) {
            result.push_back(
                {patch.id, canonical_category(plugin, patch.category), patch.enabled == FC_TRUE, true, index});
        }
    }
    for (std::size_t index = 0; index < plugin.groups.size(); ++index) {
        const auto& group = plugin.groups[index];
        if (group.configurable == FC_TRUE && applicable_group(group, plugin)) {
            result.push_back(
                {group.id, canonical_category(plugin, group.category), group.enabled == FC_TRUE, false, index});
        }
    }
    return result;
}

[[nodiscard]] bool file_participates(const catalog::PluginDefinitionRecord& plugin, bool core,
                                     std::span<const BooleanEntry> toggles) {
    return core || !toggles.empty() || std::ranges::any_of(plugin.patches, [](const auto& patch) {
               return patch.selected_support && !effective_settings(patch).empty();
           });
}

[[nodiscard]] std::optional<catalog::CategoryDefinitionRecord>
find_category(const catalog::PluginDefinitionRecord& plugin, std::string_view id) {
    const auto found = std::ranges::find_if(plugin.categories, [&](const auto& category) {
        return catalog::equal_ascii_case_insensitive(category.id, id);
    });
    return found == plugin.categories.end() ? std::nullopt : std::optional{*found};
}

void append_section(std::string& output, std::string_view section, bool& emitted) {
    if (emitted) {
        output += "\r\n";
    }
    output += '[';
    output += section;
    output += "]\r\n";
    emitted = true;
}

// Renders one complete deterministic file from the same admitted schema used for parsing and later resolution.
[[nodiscard]] std::expected<std::string, std::string> render(const catalog::PluginDefinitionRecord& plugin, bool core,
                                                             std::span<const BooleanEntry> toggles) {
    std::string output;
    bool emitted{};
    if (core) {
        // Framework options lead the file and use compiled defaults selected for the active configuration.
        append_section(output, "FusionCutter", emitted);
#if defined(NDEBUG)
        output += "LogLevel=Error\r\n";
#else
        output += "LogLevel=Debug\r\n";
#endif
        output += "MaxTraceSizeMB=512\r\n";
    }

    // Presentation order is explicit because neither registration order nor map order is framework policy.
    std::map<std::string, std::vector<const BooleanEntry*>> sections;
    for (const auto& toggle : toggles) {
        sections[std::string{toggle.category.empty() ? "General" : toggle.category}].push_back(&toggle);
    }
    std::vector<std::string> section_order;
    section_order.reserve(sections.size());
    if (sections.contains("General")) {
        section_order.emplace_back("General");
    }
    for (const auto& [section, entries] : sections) {
        (void)entries;
        if (section != "General") {
            section_order.push_back(section);
        }
    }
    std::ranges::sort(section_order.begin() + static_cast<std::ptrdiff_t>(sections.contains("General")),
                      section_order.end(), [&](const std::string& left, const std::string& right) {
                          const auto left_category = find_category(plugin, left);
                          const auto right_category = find_category(plugin, right);
                          const bool left_ordered = left_category && left_category->has_order == FC_TRUE;
                          const bool right_ordered = right_category && right_category->has_order == FC_TRUE;
                          if (left_ordered != right_ordered) {
                              return left_ordered;
                          }
                          if (left_ordered && left_category->order != right_category->order) {
                              return left_category->order < right_category->order;
                          }
                          return alphabetical(left, right);
                      });
    for (const auto& section : section_order) {
        append_section(output, section, emitted);
        auto entries = sections[section];
        std::ranges::sort(entries, [](const BooleanEntry* left, const BooleanEntry* right) {
            return alphabetical(left->id, right->id);
        });
        for (const auto* entry : entries) {
            output += entry->id;
            output += entry->default_value ? "=true\r\n" : "=false\r\n";
        }
    }

    // Patch settings follow toggle presentation and preserve schema section/key declaration order within each patch.
    std::vector<const catalog::PatchDefinitionRecord*> patches;
    for (const auto& patch : plugin.patches) {
        if (patch.selected_support && !effective_settings(patch).empty()) {
            patches.push_back(&patch);
        }
    }
    std::ranges::sort(patches, [](const auto* left, const auto* right) {
        return alphabetical(left->id, right->id);
    });
    for (const auto* patch : patches) {
        const auto settings = effective_settings(*patch);
        std::vector<std::string_view> setting_sections;
        if (std::ranges::any_of(settings, [](const auto& setting) {
                return setting.section.empty();
            })) {
            setting_sections.emplace_back();
        }
        for (const auto& setting : settings) {
            if (!setting.section.empty() && !std::ranges::any_of(setting_sections, [&](std::string_view existing) {
                    return catalog::equal_ascii_case_insensitive(existing, setting.section);
                })) {
                setting_sections.push_back(setting.section);
            }
        }
        for (const auto section : setting_sections) {
            const auto name = section.empty() ? patch->id : patch->id + "." + std::string{section};
            append_section(output, name, emitted);
            for (const auto& setting : settings) {
                if (!catalog::equal_ascii_case_insensitive(setting.section, section)) {
                    continue;
                }
                const auto value = format_setting_default(setting);
                if (setting.key.size() + value.size() + 1 > 4096) {
                    return std::unexpected("A generated setting exceeds the 4096-byte limit for one logical line");
                }
                output += setting.key;
                output += '=';
                output += value;
                output += "\r\n";
            }
        }
    }
    if (output.size() > kConfigurationFileByteCapacity) {
        return std::unexpected("The generated configuration exceeds 256 KiB");
    }
    return output;
}

// Reads an existing file once into bounded validated text; absence is distinct from a malformed or unreadable file.
[[nodiscard]] std::expected<std::optional<std::string>, std::string>
read_existing_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return std::unexpected("Could not inspect the configuration file: " + error.message());
        }
        return std::optional<std::string>{};
    }
    // Size is established before allocation so an oversized file is rejected without reading its contents.
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected("The existing configuration file could not be opened");
    }
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > kConfigurationFileByteCapacity) {
        return std::unexpected("The existing configuration file exceeds 256 KiB");
    }
    std::string content(static_cast<std::size_t>(end), '\0');
    input.seekg(0);
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!input && !content.empty()) {
        return std::unexpected("The existing configuration file could not be read completely");
    }
    // Whole-file encoding and newline rules are validated before either parser observes the text.
    if (content.contains('\0')) {
        return std::unexpected("The existing configuration file contains an embedded NUL byte");
    }
    if (!catalog::valid_utf8(content)) {
        return std::unexpected("The existing configuration file is not valid UTF-8");
    }
    for (std::size_t position = content.find('\r'); position != std::string::npos;
         position = content.find('\r', position + 1)) {
        if (position + 1 == content.size() || content[position + 1] != '\n') {
            return std::unexpected("The existing configuration file contains a lone carriage return");
        }
    }
    return std::optional<std::string>{std::move(content)};
}

[[nodiscard]] int validate_ini_entry(void*, const char*, const char*, const char*, int) {
    // The first inih pass validates syntax only; the owning pass below retains names without inih's buffer limit.
    return 1;
}

[[nodiscard]] std::expected<ParsedDocument, std::string> parse_document(std::string_view content) {
    // inih remains the syntax authority. The second pass preserves section names beyond inih's private 49-byte
    // scratch buffer, which is necessary for the specified 64-byte PatchId plus qualified SectionId form.
    const auto parse_result = ini_parse_string_length(content.data(), content.size(), &validate_ini_entry, nullptr);
    if (parse_result != 0) {
        const auto location = parse_result > 0 ? " at line " + std::to_string(parse_result) : std::string{};
        return std::unexpected("The existing configuration file is malformed" + location);
    }
    if (content.starts_with("\xef\xbb\xbf")) {
        content.remove_prefix(3);
    }
    // The owning pass records original spelling and line numbers while applying only the supported scalar subset.
    ParsedDocument result;
    std::string current_section;
    std::size_t line_number{};
    while (!content.empty()) {
        ++line_number;
        const auto newline = content.find('\n');
        auto line = content.substr(0, newline);
        content = newline == std::string_view::npos ? std::string_view{} : content.substr(newline + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        const auto stripped = trim(line);
        if (stripped.empty() || stripped.front() == ';' || stripped.front() == '#') {
            continue;
        }
        if (stripped.front() == '[') {
            const auto close = stripped.find(']');
            if (close == std::string_view::npos || !trim(stripped.substr(close + 1)).empty()) {
                return std::unexpected("The existing configuration file is malformed at line " +
                                       std::to_string(line_number));
            }
            current_section = stripped.substr(1, close - 1);
            result.sections.emplace_back(current_section, line_number);
            continue;
        }
        const auto separator = stripped.find_first_of("=:");
        if (separator == std::string_view::npos) {
            return std::unexpected("The existing configuration file is malformed at line " +
                                   std::to_string(line_number));
        }
        const auto key = trim(stripped.substr(0, separator));
        const auto value = trim(stripped.substr(separator + 1));
        const bool quoted = value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                                                  (value.front() == '"' && value.back() == '"'));
        if (key.empty() || quoted) {
            return std::unexpected("The existing configuration file uses unsupported scalar syntax at line " +
                                   std::to_string(line_number));
        }
        result.entries.push_back({current_section, std::string{key}, std::string{value}, line_number});
    }
    return result;
}

[[nodiscard]] CreateResult create_file(const std::filesystem::path& path, std::string_view content,
                                       std::string& error) {
    // CREATE_NEW is the commit point: a racing writer wins without this process truncating user configuration.
    const auto handle =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            return CreateResult::AlreadyExists;
        }
        error = "The missing configuration file could not be created (Windows error " + std::to_string(code) + ')';
        return CreateResult::Failed;
    }
    DWORD written{};
    const bool write_success =
        content.size() <= std::numeric_limits<DWORD>::max() &&
        WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) != 0 &&
        written == content.size();
    const auto write_error = write_success ? ERROR_SUCCESS : GetLastError();
    const bool close_success = CloseHandle(handle) != 0;
    const auto close_error = close_success ? ERROR_SUCCESS : GetLastError();
    const bool success = write_success && close_success;
    if (success) {
        return CreateResult::Created;
    }
    // A failed first writer removes its incomplete artifact so a later run never parses partial generated content.
    const auto code = write_success ? close_error : write_error;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    error =
        "The generated configuration file could not be written completely (Windows error " + std::to_string(code) + ')';
    return CreateResult::Failed;
}

[[nodiscard]] std::optional<FC_LogLevel> parse_log_level(std::string_view value) noexcept {
    constexpr std::array values{std::pair{"Off", FC_LOG_OFF}, std::pair{"Error", FC_LOG_ERROR},
                                std::pair{"Warning", FC_LOG_WARNING}, std::pair{"Info", FC_LOG_INFO},
                                std::pair{"Debug", FC_LOG_DEBUG}};
    const auto found = std::ranges::find_if(values, [&](const auto& candidate) {
        return catalog::equal_ascii_case_insensitive(candidate.first, value);
    });
    return found == values.end() ? std::nullopt : std::optional{found->second};
}

[[nodiscard]] std::optional<std::string> read_environment(std::string_view name, std::string& error) {
    if (name.empty()) {
        return std::nullopt;
    }
    // Preserve the Windows distinction between an absent variable, a present empty value, and an API failure.
    SetLastError(ERROR_SUCCESS);
    const std::string terminated{name};
    const auto required = GetEnvironmentVariableA(terminated.c_str(), nullptr, 0);
    if (required == 0) {
        const auto code = GetLastError();
        if (code == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        if (code == ERROR_SUCCESS) {
            return std::string{};
        }
        error = "Environment variable could not be read (Windows error " + std::to_string(code) + ')';
        return std::nullopt;
    }

    std::string value(required, '\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const auto size = GetEnvironmentVariableA(terminated.c_str(), value.data(), static_cast<DWORD>(value.size()));
        if (size == 0) {
            const auto code = GetLastError();
            if (code == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            if (code == ERROR_SUCCESS) {
                return std::string{};
            }
            error = "Environment variable could not be read (Windows error " + std::to_string(code) + ')';
            return std::nullopt;
        }
        if (size < value.size()) {
            value.resize(size);
            return value;
        }
        // Retry with the new required size if another thread changed the process environment between calls.
        value.resize(size);
    }
}

void add_diagnostic(PluginConfiguration& configuration, std::size_t line, std::string message) {
    // The bounded source file limits diagnostic work; ordinary reporting owns output-size constraints.
    configuration.diagnostics.push_back({line, std::move(message)});
}

void store_value(std::optional<StoredValue>& destination, const ParsedEntry& entry,
                 PluginConfiguration& configuration) {
    // Using the last value matches the parser contract, but every duplicate remains visible as a diagnostic.
    if (destination) {
        add_diagnostic(configuration, entry.line,
                       "Duplicate [" + entry.section + "]." + entry.key + "; last value wins");
    }
    destination = StoredValue{entry.value, entry.line};
}

[[nodiscard]] bool known_section(const catalog::PluginDefinitionRecord& plugin, bool core,
                                 std::span<const BooleanEntry> toggles, std::string_view section) {
    if (core && catalog::equal_ascii_case_insensitive(section, "FusionCutter")) {
        return true;
    }
    if (std::ranges::any_of(toggles, [&](const BooleanEntry& toggle) {
            const auto expected = toggle.category.empty() ? std::string_view{"General"} : toggle.category;
            return catalog::equal_ascii_case_insensitive(expected, section);
        })) {
        return true;
    }
    return std::ranges::any_of(plugin.patches, [&](const auto& patch) {
        if (!patch.selected_support) {
            return false;
        }
        return std::ranges::any_of(effective_settings(patch), [&](const auto& setting) {
            const auto expected = setting.section.empty() ? patch.id : patch.id + "." + setting.section;
            return catalog::equal_ascii_case_insensitive(expected, section);
        });
    });
}

// Matches parsed values only to the admitted schema, retaining duplicates and unknown input as bounded diagnostics.
void apply_document(const ParsedDocument& document, const catalog::PluginDefinitionRecord& plugin, bool core,
                    std::span<const BooleanEntry> toggles, ConfigurationLoadResult& result) {
    // Diagnose each unknown section once even when it contains several unrecognized entries.
    std::set<std::string> unknown_sections;
    for (const auto& [section, line] : document.sections) {
        if (!known_section(plugin, core, toggles, section) &&
            unknown_sections.insert(catalog::fold_ascii(section)).second) {
            add_diagnostic(result.configuration, line, "Unknown configuration section [" + section + ']');
        }
    }
    std::optional<StoredValue> log_level;
    std::optional<StoredValue> max_trace_size;
    // All recognized destinations are fixed before parsing; unknown values can never extend the effective schema.
    for (const auto& entry : document.entries) {
        if (core && catalog::equal_ascii_case_insensitive(entry.section, "FusionCutter")) {
            if (catalog::equal_ascii_case_insensitive(entry.key, "LogLevel")) {
                store_value(log_level, entry, result.configuration);
            } else if (catalog::equal_ascii_case_insensitive(entry.key, "MaxTraceSizeMB")) {
                store_value(max_trace_size, entry, result.configuration);
            } else {
                add_diagnostic(result.configuration, entry.line, "Unknown setting [FusionCutter]." + entry.key);
            }
            continue;
        }
        const auto toggle = std::ranges::find_if(toggles, [&](const BooleanEntry& candidate) {
            const auto section = candidate.category.empty() ? std::string_view{"General"} : candidate.category;
            return catalog::equal_ascii_case_insensitive(section, entry.section) &&
                   catalog::equal_ascii_case_insensitive(candidate.id, entry.key);
        });
        if (toggle != toggles.end()) {
            auto& destination = toggle->patch ? result.configuration.patches[toggle->index].toggle
                                              : result.configuration.groups[toggle->index].toggle;
            store_value(destination, entry, result.configuration);
            continue;
        }
        bool setting_found{};
        for (std::size_t patch_index = 0; patch_index < plugin.patches.size() && !setting_found; ++patch_index) {
            const auto& patch = plugin.patches[patch_index];
            const auto settings = effective_settings(patch);
            for (std::size_t setting_index = 0; setting_index < settings.size(); ++setting_index) {
                const auto& setting = settings[setting_index];
                const auto section = setting.section.empty() ? patch.id : patch.id + "." + setting.section;
                if (catalog::equal_ascii_case_insensitive(section, entry.section) &&
                    catalog::equal_ascii_case_insensitive(setting.key, entry.key)) {
                    store_value(result.configuration.patches[patch_index].settings[setting_index].ini, entry,
                                result.configuration);
                    setting_found = true;
                    break;
                }
            }
        }
        if (!setting_found) {
            if (known_section(plugin, core, toggles, entry.section)) {
                add_diagnostic(result.configuration, entry.line,
                               "Unknown setting [" + entry.section + "]." + entry.key);
            } else if (unknown_sections.insert(catalog::fold_ascii(entry.section)).second) {
                add_diagnostic(result.configuration, entry.line,
                               "Unknown configuration section [" + entry.section + ']');
            }
        }
    }

    // Framework values are independently recoverable: each invalid scalar diagnoses and falls back to its default.
    if (core) {
        auto options = *result.framework;
        if (log_level) {
            const auto parsed = parse_log_level(log_level->value);
            if (parsed) {
                options.log_level = *parsed;
            } else {
                add_diagnostic(result.configuration, log_level->line,
                               "Invalid [FusionCutter].LogLevel; compiled default used");
            }
        }
        if (max_trace_size) {
            std::uint32_t value{};
            const auto [end, error] = std::from_chars(
                max_trace_size->value.data(), max_trace_size->value.data() + max_trace_size->value.size(), value);
            if (error == std::errc{} && end == max_trace_size->value.data() + max_trace_size->value.size() &&
                value <= 4096) {
                options.max_trace_size_mb = value;
            } else {
                add_diagnostic(result.configuration, max_trace_size->line,
                               "Invalid [FusionCutter].MaxTraceSizeMB; compiled default used");
            }
        }
        result.framework = options;
    }
}

} // namespace

std::filesystem::path ConfigurationPaths::config_directory() const {
    return installation_directory / "config";
}

std::filesystem::path ConfigurationPaths::plugin_file(std::string_view plugin_id) const {
    return config_directory() / ("FC." + std::string{plugin_id} + ".ini");
}

std::expected<std::string, std::string> render_configuration(const catalog::PluginDefinitionRecord& plugin, bool core) {
    const auto toggles = boolean_entries(plugin);
    return render(plugin, core, toggles);
}

std::expected<ConfigurationLoadResult, std::string> load_configuration(const catalog::PluginDefinitionRecord& plugin,
                                                                       bool core, const ConfigurationPaths& paths) {
    // Allocate the carrier shaped like the plugin definition before reading any mutable external source.
    const auto toggles = boolean_entries(plugin);
    ConfigurationLoadResult result;
    result.configuration.plugin_id = plugin.id;
    result.configuration.patches.resize(plugin.patches.size());
    result.configuration.groups.resize(plugin.groups.size());
    if (core) {
        result.framework.emplace();
    }
    // Capture environment sources in this pass so later resolution does not read mutable process state.
    for (std::size_t patch_index = 0; patch_index < plugin.patches.size(); ++patch_index) {
        const auto settings = effective_settings(plugin.patches[patch_index]);
        result.configuration.patches[patch_index].settings.resize(settings.size());
        for (std::size_t setting_index = 0; setting_index < settings.size(); ++setting_index) {
            auto& raw = result.configuration.patches[patch_index].settings[setting_index];
            std::string environment_error;
            raw.environment = read_environment(settings[setting_index].environment, environment_error);
            if (!environment_error.empty()) {
                raw.environment_error = std::move(environment_error);
            }
        }
    }

    // Nonparticipating plugins retain captured environment values but perform no filesystem work.
    result.configuration.file_expected = file_participates(plugin, core, toggles);
    if (!result.configuration.file_expected) {
        return result;
    }
    const auto path = paths.plugin_file(plugin.id);
    auto content = read_existing_file(path);
    if (!content) {
        return std::unexpected(configuration_error(path, std::move(content.error())));
    }
    if (!*content) {
        // Generation is prepared entirely in memory before the config directory or destination file is created.
        auto generated = render(plugin, core, toggles);
        if (!generated) {
            return std::unexpected(configuration_error(path, std::move(generated.error())));
        }
        std::error_code directory_error;
        std::filesystem::create_directories(paths.config_directory(), directory_error);
        if (directory_error) {
            result.configuration.output_error =
                configuration_error(path, "The config directory could not be created: " + directory_error.message());
            return result;
        }
        if (core) {
            std::error_code legacy_error;
            const bool legacy_client =
                std::filesystem::exists(paths.installation_directory / "FusionCutter.ini", legacy_error);
            legacy_error.clear();
            const bool legacy_server =
                std::filesystem::exists(paths.installation_directory / "FusionCutter-Server.ini", legacy_error);
            if (legacy_client || legacy_server) {
                add_diagnostic(result.configuration, 0, "Legacy root configuration was not imported");
            }
        }
        std::string output_error;
        switch (create_file(path, *generated, output_error)) {
        case CreateResult::Created:
            result.configuration.file_created = true;
            return result;
        case CreateResult::Failed:
            result.configuration.output_error = configuration_error(path, std::move(output_error));
            return result;
        case CreateResult::AlreadyExists:
            // A concurrent creator owns the file; read that winner exactly once without overwriting it.
            content = read_existing_file(path);
            if (!content || !*content) {
                return std::unexpected(configuration_error(
                    path, content ? "The configuration file disappeared during creation" : std::move(content.error())));
            }
            break;
        }
    }
    // Existing files, including one created by a racing process, share the same validation and schema path.
    auto parsed = parse_document(**content);
    if (!parsed) {
        return std::unexpected(configuration_error(path, std::move(parsed.error())));
    }
    apply_document(*parsed, plugin, core, toggles, result);
    return result;
}

} // namespace fc::config
