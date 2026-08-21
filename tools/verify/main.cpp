#include "catalog/plugin_discovery.hpp"
#include "targets/mapped_file.hpp"
#include "targets/recognition.hpp"
#include "targets/target_profiles.hpp"

#include <FusionCutter/Testing.hpp>

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Target recognition enriches each image path with the exact profile passed into the public testing API.
struct ImageArgument {
    fc::TargetImage image;
    std::filesystem::path path;
    std::string profile;
};

// Parsed arguments preserve repeatable inputs while making singular role/configuration options explicit.
struct Arguments {
    std::optional<fc::HostRole> role;
    std::vector<std::pair<fc::TargetImage, std::filesystem::path>> images;
    std::vector<std::filesystem::path> plugins;
    std::optional<std::filesystem::path> configuration;
    std::vector<std::string> checks;
};

// Command vocabulary uses the same case-insensitive Windows identity rules as discovery and target naming.
[[nodiscard]] bool same_text(std::wstring_view left, std::wstring_view right) noexcept {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::optional<fc::TargetImage> parse_image(std::wstring_view value) noexcept {
    if (same_text(value, L"Game")) {
        return fc::TargetImage::Game;
    }
    if (same_text(value, L"Bootstrap")) {
        return fc::TargetImage::Bootstrap;
    }
    if (same_text(value, L"GalaxyPeer")) {
        return fc::TargetImage::GalaxyPeer;
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<Arguments, std::string> parse_arguments(int count, wchar_t* values[]) {
    // Parse the complete fixed command vocabulary and reject duplicates before opening any caller-supplied path.
    Arguments result;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view option{values[index]};
        if (index + 1 >= count) {
            return std::unexpected("Every verifier option requires a value");
        }
        const std::wstring_view value{values[++index]};
        if (option == L"--role") {
            if (result.role) {
                return std::unexpected("--role may be supplied only once");
            }
            if (same_text(value, L"client")) {
                result.role = fc::HostRole::Client;
            } else if (same_text(value, L"server")) {
                result.role = fc::HostRole::Server;
            } else {
                return std::unexpected("--role must be client or server");
            }
        } else if (option == L"--image") {
            const auto separator = value.find(L'=');
            const auto image = parse_image(value.substr(0, separator));
            if (separator == std::wstring_view::npos || !image || separator + 1 == value.size()) {
                return std::unexpected("--image must use Image=Path with a supported image ID");
            }
            if (std::ranges::any_of(result.images, [&](const auto& existing) {
                    return existing.first == *image;
                })) {
                return std::unexpected("A target image ID may be supplied only once");
            }
            result.images.emplace_back(*image, std::filesystem::path{value.substr(separator + 1)});
        } else if (option == L"--plugin") {
            result.plugins.emplace_back(value);
        } else if (option == L"--config-dir") {
            if (result.configuration) {
                return std::unexpected("--config-dir may be supplied only once");
            }
            result.configuration.emplace(value);
        } else if (option == L"--check") {
            const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                                      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (required <= 0) {
                return std::unexpected("A --check patch ID is not valid UTF-8");
            }
            std::string id(static_cast<std::size_t>(required), '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), id.data(),
                                required, nullptr, nullptr);
            result.checks.push_back(std::move(id));
        } else {
            return std::unexpected("Unknown verifier option");
        }
    }
    // Validate the complete invocation only after order-independent options have been collected.
    if (!result.role || result.images.empty()) {
        return std::unexpected("--role and at least one --image are required");
    }
    return result;
}

// Stable public state names make verifier output suitable for both humans and lightweight automation.
[[nodiscard]] std::string_view state_name(fc::test::PatchState state) noexcept {
    switch (state) {
    case fc::test::PatchState::Disabled:
        return "Disabled";
    case fc::test::PatchState::NotApplicable:
        return "NotApplicable";
    case fc::test::PatchState::WaitingForImage:
        return "WaitingForImage";
    case fc::test::PatchState::Ready:
        return "Ready";
    case fc::test::PatchState::Skipped:
        return "Skipped";
    case fc::test::PatchState::Failed:
        return "Failed";
    }
    return "Failed";
}

[[nodiscard]] std::expected<std::pair<fc::TargetLayout, std::vector<ImageArgument>>, std::string>
recognize_images(const Arguments& arguments) {
    // Real files are recognized through the production fingerprint catalog before entering synthetic Scenario input.
    std::optional<fc::TargetLayout> layout;
    std::vector<ImageArgument> result;
    result.reserve(arguments.images.size());
    for (const auto& [declared_image, path] : arguments.images) {
        auto mapped = fc::targets::map_pe_file(path);
        if (!mapped) {
            return std::unexpected(path.string() + ": " + mapped.error());
        }
        auto recognized = fc::targets::recognize_owned_mapped_image(path.filename().string(), std::move(*mapped),
                                                                    fc::targets::known_image_profiles());
        if (!recognized) {
            return std::unexpected(path.string() + ": " + recognized.error().message);
        }
        const auto& info = recognized->view().info();
        if (info.image != static_cast<FC_TargetImage>(declared_image)) {
            return std::unexpected(path.string() + ": the recognized image ID differs from --image");
        }
        const auto* profile = fc::targets::find_image_profile(info.profile);
        if (profile == nullptr) {
            return std::unexpected(path.string() + ": the recognized profile is unavailable");
        }
        // Every supplied image must contribute to one coherent target rather than forming a mixed-layout Scenario.
        const auto current_layout = static_cast<fc::TargetLayout>(profile->layout);
        if (layout && *layout != current_layout) {
            return std::unexpected("The supplied images do not belong to one target layout");
        }
        layout = current_layout;
        result.push_back({declared_image, path, std::string{profile->id}});
    }
    return std::pair{*layout, std::move(result)};
}

[[nodiscard]] bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    // Plugin admission uses normalized Windows path identity rather than a potentially colliding plugin ID.
    const auto left_text = left.native();
    const auto right_text = right.native();
    return CompareStringOrdinal(left_text.data(), static_cast<int>(left_text.size()), right_text.data(),
                                static_cast<int>(right_text.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

int wmain(int count, wchar_t* values[]) {
    // Frontend errors remain distinct from validation failures so command misuse gets the conventional exit code 2.
    const auto parsed = parse_arguments(count, values);
    if (!parsed) {
        std::cerr << "Argument error: " << parsed.error() << "\n\n"
                  << "Usage: FusionCutterVerify --role client|server --image Image=Path "
                     "[--image Image=Path ...] [--plugin Path ...] [--config-dir Path] [--check PatchId ...]\n";
        return 2;
    }

    auto recognized = recognize_images(*parsed);
    if (!recognized) {
        std::cerr << "Image error: " << recognized.error() << '\n';
        return 1;
    }
    auto normalized_plugins = fc::catalog::normalize_plugin_paths(parsed->plugins);
    if (!normalized_plugins) {
        std::cerr << "Plugin input error: " << normalized_plugins.error() << '\n';
        return 2;
    }

    // After recognizing the real file, the public testing API owns admission, configuration, and planning.
    fc::test::Scenario scenario{recognized->first, *parsed->role};
    for (const auto& image : recognized->second) {
        scenario.add_image(image.image, image.profile, image.path);
    }
    for (const auto& plugin : *normalized_plugins) {
        scenario.add_plugin(plugin);
    }
    if (parsed->configuration) {
        scenario.use_config(*parsed->configuration);
    }
    const auto validated = scenario.validate();
    if (!validated) {
        std::cerr << "Scenario error: " << validated.error().operation << ": " << validated.error().message << '\n';
        return 1;
    }

    // Requested plugin paths must survive admission even when another candidate exposes the same declared ID.
    bool failed{};
    std::cout << "Plugins:\n";
    for (const auto& plugin : validated->plugins()) {
        std::cout << "  " << (plugin.path ? plugin.path->filename().string() : plugin.id.value_or("Built-in")) << ": "
                  << (plugin.admitted ? "Admitted" : "Rejected") << '\n';
        if (plugin.reason) {
            std::cout << "    Reason: " << *plugin.reason << '\n';
        }
    }
    for (const auto& requested : *normalized_plugins) {
        const auto admitted = std::ranges::any_of(validated->plugins(), [&](const auto& plugin) {
            return plugin.admitted && plugin.path && same_path(*plugin.path, requested);
        });
        if (!admitted) {
            std::cerr << "Requested plugin was not admitted: " << requested.string() << '\n';
            failed = true;
        }
    }

    // Any selected terminal failure invalidates verification; waiting remains valid unless explicitly checked.
    std::cout << "Patches:\n";
    for (const auto& patch : validated->patches()) {
        std::cout << "  " << patch.id << ": " << state_name(patch.state) << '\n';
        if (patch.reason) {
            std::cout << "    Reason: " << *patch.reason << '\n';
        }
        if (patch.state == fc::test::PatchState::Failed || patch.state == fc::test::PatchState::Skipped) {
            failed = true;
        }
    }
    // Named checks are stronger assertions and must resolve to an admitted patch in the Ready state.
    for (const auto& check : parsed->checks) {
        const auto* patch = validated->find_patch(check);
        if (patch == nullptr || patch->state != fc::test::PatchState::Ready) {
            std::cerr << "Check did not reach the Ready state: " << check << '\n';
            failed = true;
        }
    }

    std::cout << (failed ? "Verification failed.\n" : "Verification passed.\n");
    return failed ? 1 : 0;
}
