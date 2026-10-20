#include "configuration.hpp"

#include "storage.hpp"

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>

namespace fc::runtime {

using namespace configuration_detail;

std::string_view log_level_name(FC_LogLevel level) noexcept {
    switch (level) {
    case FC_LOG_OFF:
        return "Off";
    case FC_LOG_ERROR:
        return "Error";
    case FC_LOG_INFO:
        return "Info";
    case FC_LOG_DEBUG:
        return "Debug";
    default:
        return "Warning";
    }
}

std::expected<ConfigurationPaths, Diagnostic> runtime_configuration_paths() {
    // Locate this function's containing module so configuration uses that module's directory.
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&runtime_configuration_paths), &module)) {
        return std::unexpected(Diagnostic{"runtime", "Could not identify the module containing the runtime", GetLastError()});
    }

    // Grow the UTF-16 buffer until Windows returns the complete module path.
    std::wstring path(256, L'\0');
    for (;;) {
        const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!size) {
            return std::unexpected(Diagnostic{"runtime", "Could not read the runtime module's path", GetLastError()});
        }
        if (size < path.size()) {
            path.resize(size);
            return ConfigurationPaths{std::filesystem::path{path}.parent_path()};
        }
        path.resize(path.size() * 2);
    }
}

std::expected<RuntimeSettings, Diagnostic> load_runtime_configuration(const ConfigurationPaths &paths) {
    RuntimeSettings settings;
    constexpr std::uint64_t kBytesPerMb = 1048576;
    FC_Value log{};
    log.kind = FC_SCALAR_TEXT;
    log.value.text = fc::detail::text(log_level_name(settings.log_level));

    FC_Value trace{};
    trace.kind = FC_SCALAR_UNSIGNED;
    trace.value.unsigned_value = settings.trace_file_bytes / kBytesPerMb;
    const std::array entries{GeneratedEntry{"FusionCutter", "LogLevel", "Log level: Off, Error, Warning, Info or Debug.", "", log},
                             GeneratedEntry{"FusionCutter", "MaxTraceSizeMB",
                                            "Maximum trace file size in MB (1 MB = 1,048,576 bytes).\nSet to 0 to disable recording.", "",
                                            trace}};

    const auto path = paths.directory / "FusionCutter.ini";
    const auto owner = path_text(path);
    auto document = load_file(path, entries, paths.write_defaults, settings.warnings);
    if (!document) {
        return std::unexpected(document.error());
    }

    // Unknown sections and keys remain in the file and add warnings without rejecting known settings.
    warn_unknown(*document, entries, settings.warnings, owner);

    // Match the named level ASCII case-insensitively, retaining Warning for an invalid spelling.
    if (const auto *entry = find(*document, "FusionCutter", "LogLevel")) {
        constexpr std::array levels{FC_LOG_OFF, FC_LOG_ERROR, FC_LOG_WARNING, FC_LOG_INFO, FC_LOG_DEBUG};
        const auto level = std::ranges::find_if(levels, [&](auto value) {
            return fc::detail::equal_id(entry->value, log_level_name(value));
        });
        if (level != levels.end()) {
            settings.log_level = *level;
        } else {
            settings.warnings.push_back({owner, "Invalid LogLevel; using the default value Warning"});
        }
    }

    // Convert MB to the internal byte budget only when multiplication fits; rejected input retains the declared default.
    if (const auto *entry = find(*document, "FusionCutter", "MaxTraceSizeMB")) {
        auto parsed = parse_scalar(entry->value, FC_SCALAR_UNSIGNED, owner);
        if (parsed && std::get<std::uint64_t>(parsed->value) <= std::numeric_limits<std::uint64_t>::max() / kBytesPerMb) {
            settings.trace_file_bytes = std::get<std::uint64_t>(parsed->value) * kBytesPerMb;
        } else {
            settings.warnings.push_back({owner, "Invalid MaxTraceSizeMB; using the default value 512 MB"});
        }
    }

    return settings;
}

} // namespace fc::runtime
