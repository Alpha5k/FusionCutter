#include "plugin_discovery.hpp"

#include "pe_file.hpp"

#include <Windows.h>

#include <algorithm>
#include <system_error>

namespace fc::catalog {
namespace {

// Discovery ordering follows Windows ordinal comparison, with exact case as the deterministic tie-breaker.
[[nodiscard]] int compare_ordinal(std::wstring_view left, std::wstring_view right, bool ignore_case) noexcept {
    const auto result = CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                             static_cast<int>(right.size()), ignore_case ? TRUE : FALSE);
    if (result == CSTR_LESS_THAN) {
        return -1;
    }
    if (result == CSTR_GREATER_THAN) {
        return 1;
    }
    return 0;
}

[[nodiscard]] bool filename_less(const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    const auto left_name = left.filename().native();
    const auto right_name = right.filename().native();
    const auto insensitive = compare_ordinal(left_name, right_name, true);
    if (insensitive != 0) {
        return insensitive < 0;
    }
    const auto sensitive = compare_ordinal(left_name, right_name, false);
    if (sensitive != 0) {
        return sensitive < 0;
    }
    const auto left_path = left.native();
    const auto right_path = right.native();
    const auto path_insensitive = compare_ordinal(left_path, right_path, true);
    return path_insensitive != 0 ? path_insensitive < 0 : compare_ordinal(left_path, right_path, false) < 0;
}

[[nodiscard]] bool dll_extension(const std::filesystem::path& path) noexcept {
    return compare_ordinal(path.extension().native(), L".dll", true) == 0;
}

[[nodiscard]] FC_Architecture process_architecture() noexcept {
    return sizeof(void*) == 4 ? FC_ARCH_X86 : FC_ARCH_X64;
}

} // namespace

std::expected<std::vector<std::filesystem::path>, std::string>
normalize_plugin_paths(std::span<const std::filesystem::path> paths) {
    // Resolve identity once at the frontend so later admission never interprets relative paths against changed state.
    std::vector<std::filesystem::path> result;
    result.reserve(paths.size());
    for (const auto& input : paths) {
        std::error_code error;
        auto normalized = std::filesystem::absolute(input, error).lexically_normal();
        if (error || normalized.empty()) {
            return std::unexpected("An explicit plugin path could not be normalized to an absolute path");
        }
        const auto duplicate = std::ranges::any_of(result, [&](const auto& existing) {
            return compare_ordinal(existing.native(), normalized.native(), true) == 0;
        });
        if (duplicate) {
            return std::unexpected("The explicit plugin list contains a duplicate normalized Windows path");
        }
        result.push_back(std::move(normalized));
    }
    // Production filename ordering also determines capacity ordinals for this explicit candidate set.
    std::ranges::sort(result, filename_less);
    return result;
}

DiscoveryResult discover_plugin_paths(std::span<const std::filesystem::path> paths) {
    // Inspect exactly the supplied files; unlike directory discovery, a missing query export is an explicit rejection.
    DiscoveryResult result;
    std::size_t query_candidate_count{};
    for (const auto& path : paths) {
        auto facts = inspect_plugin_binary(path);
        if (!facts) {
            result.rejections.push_back(
                {.path = path, .stage = AdmissionStage::Discovery, .reason = std::move(facts.error())});
            continue;
        }
        if (facts->query_export == QueryExportKind::Missing) {
            result.rejections.push_back({.path = path,
                                         .stage = AdmissionStage::Discovery,
                                         .reason = "The explicit plugin has no FusionCutter_QueryPlugin export"});
            continue;
        }

        // Capacity ordinals count exact-name query exports, including candidates rejected later.
        const auto order = query_candidate_count++;
        if (order >= kExternalCandidateCapacity) {
            result.rejections.push_back({.path = path,
                                         .stage = AdmissionStage::Capacity,
                                         .reason = "The external plugin candidate capacity was exceeded"});
        } else if (facts->query_export == QueryExportKind::Forwarded) {
            result.rejections.push_back({.path = path,
                                         .stage = AdmissionStage::Discovery,
                                         .reason = "The plugin query export is forwarded and will not be executed"});
        } else if (facts->architecture != process_architecture()) {
            result.rejections.push_back({.path = path,
                                         .stage = AdmissionStage::Architecture,
                                         .reason = "The plugin architecture does not match this Fusion Cutter build"});
        } else {
            result.candidates.push_back({path, order});
        }
    }
    return result;
}

DiscoveryResult discover_plugins(const std::filesystem::path& plugins_directory) {
    DiscoveryResult result;
    std::error_code error;
    if (!std::filesystem::exists(plugins_directory, error)) {
        return result;
    }
    if (error || !std::filesystem::is_directory(plugins_directory, error)) {
        result.rejections.push_back(
            {.stage = AdmissionStage::Discovery, .reason = "The plugins path could not be enumerated as a directory"});
        return result;
    }

    // Snapshot only immediate regular DLL names, then sort them with stable Windows ordinal semantics.
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator iterator{plugins_directory, error}, end; iterator != end && !error;
         iterator.increment(error)) {
        std::error_code file_error;
        if (iterator->is_regular_file(file_error) && !file_error && dll_extension(iterator->path())) {
            files.push_back(plugins_directory / iterator->path().filename());
        }
    }
    if (error) {
        result.rejections.push_back(
            {.stage = AdmissionStage::Discovery, .reason = "The plugins directory scan did not complete"});
        return result;
    }
    std::ranges::sort(files, filename_less);

    // The capacity ordinal counts every exactly named query export before forwarder or architecture rejection.
    std::size_t query_candidate_count{};
    for (const auto& path : files) {
        auto facts = inspect_plugin_binary(path);
        if (!facts) {
            result.rejections.push_back(
                {.path = path, .stage = AdmissionStage::Discovery, .reason = std::move(facts.error())});
            continue;
        }
        if (facts->query_export == QueryExportKind::Missing) {
            continue;
        }

        const auto order = query_candidate_count++;
        if (order >= kExternalCandidateCapacity) {
            result.rejections.push_back({.path = path,
                                         .stage = AdmissionStage::Capacity,
                                         .reason = "The external plugin candidate capacity was exceeded"});
            continue;
        }
        if (facts->query_export == QueryExportKind::Forwarded) {
            result.rejections.push_back({.path = path,
                                         .stage = AdmissionStage::Discovery,
                                         .reason = "The plugin query export is forwarded and will not be executed"});
            continue;
        }
        if (facts->architecture != process_architecture()) {
            result.rejections.push_back({.path = path,
                                         .stage = AdmissionStage::Architecture,
                                         .reason = "The plugin architecture does not match this Fusion Cutter build"});
            continue;
        }
        result.candidates.push_back({path, order});
    }
    return result;
}

} // namespace fc::catalog
