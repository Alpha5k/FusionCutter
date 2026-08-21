#pragma once

#include <FusionCutter/Abi.h>

#include <expected>
#include <filesystem>
#include <string>

namespace fc::catalog {

// Query classification distinguishes harmless non-plugins from exports that must be rejected before execution.
enum class QueryExportKind {
    Missing,
    Direct,
    Forwarded,
};

// Discovery retains only the two PE facts needed to check architecture and the required direct export.
struct PluginBinaryFacts {
    FC_Architecture architecture{};
    QueryExportKind query_export{};
};

// Boundedly inspects the on-disk PE and classifies the exact query export as direct or forwarded before code executes.
[[nodiscard]] std::expected<PluginBinaryFacts, std::string> inspect_plugin_binary(const std::filesystem::path& path);

} // namespace fc::catalog
