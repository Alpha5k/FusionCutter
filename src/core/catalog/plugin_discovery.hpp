#pragma once

#include "catalog_types.hpp"

#include <filesystem>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fc::catalog {

// Carries one file that passed discovery far enough to enter bounded admission in deterministic order.
struct ExternalCandidate {
    std::filesystem::path path;
    // Counts exactly named query exports before forwarder or architecture rejection, as required by the candidate cap.
    std::size_t discovery_order{};
};

// Discovery reports both admissible files and per-file rejections so one bad plugin does not hide the others.
struct DiscoveryResult {
    std::vector<ExternalCandidate> candidates;
    std::vector<RejectionRecord> rejections;
};

// Performs one non-recursive, ordinal filename-sorted scan and returns only direct, architecture-matching candidates.
[[nodiscard]] DiscoveryResult discover_plugins(const std::filesystem::path& plugins_directory);

// Explicit test/tool inputs use the same Windows path identity and filename ordering as production discovery.
[[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string>
normalize_plugin_paths(std::span<const std::filesystem::path> paths);

// Inspects a normalized explicit set without scanning or substituting files from the containing directories.
[[nodiscard]] DiscoveryResult discover_plugin_paths(std::span<const std::filesystem::path> paths);

} // namespace fc::catalog
