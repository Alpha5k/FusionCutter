#include "mapped_image.hpp"
#include "verifier.hpp"

#include "catalog/catalog.hpp"
#include "catalog/generated_catalog.hpp"

#include <FusionCutter/target.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

using namespace fusioncutter;

[[nodiscard]] constexpr Architecture current_architecture() noexcept {
    return sizeof(void*) == 4 ? Architecture::X86 : Architecture::X64;
}

[[nodiscard]] constexpr std::string_view layout_name(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
        return "SteamRetail";
    case TargetLayout::GOGRetail:
        return "GOGRetail";
    case TargetLayout::Aspyr:
        return "Aspyr";
    case TargetLayout::ModTools:
        return "ModTools";
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::string_view image_name(TargetImage image) noexcept {
    switch (image) {
    case TargetImage::Game:
        return "Game";
    case TargetImage::Bootstrap:
        return "Bootstrap";
    case TargetImage::GalaxyPeer:
        return "GalaxyPeer";
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::string_view role_name(HostRole role) noexcept {
    return role == HostRole::Client ? "Client" : "Server";
}

} // namespace

int wmain(int argument_count, wchar_t* arguments[]) {
    if (argument_count < 2) {
        std::cerr << "Usage: FusionCutter-Verify <supported-image> [supported-image ...]\n";
        return 2;
    }

    try {
        auto catalog = fusioncutter::catalog::initialize_catalog(fusioncutter::catalog::generated_catalog_entries(),
                                                                 {current_architecture(), true, true});
        if (!catalog.has_value()) {
            std::cerr << "Catalog error: " << catalog.error().message << '\n';
            return 1;
        }

        std::size_t image_count{};
        std::size_t plan_count{};
        bool failed{};
        for (int index = 1; index < argument_count; ++index) {
            const std::filesystem::path path{arguments[index]};
            auto image = fusioncutter::verify::MappedImage::load(path);
            if (!image.has_value()) {
                std::cerr << path << ": " << image.error() << '\n';
                failed = true;
                continue;
            }

            auto verified = fusioncutter::verify::verify_supported_image(path, *image, *catalog);
            if (!verified.has_value()) {
                std::cerr << path << ": " << verified.error() << '\n';
                failed = true;
                continue;
            }

            ++image_count;
            plan_count += verified->plans.size();
            std::cout << path << ": " << layout_name(verified->layout) << ' ' << image_name(verified->identity) << "\n";
            if (verified->plans.empty()) {
                std::cout << "  No applicable patch plans.\n";
                continue;
            }
            for (const auto& plan : verified->plans) {
                std::cout << "  " << plan.patch_id << " (" << role_name(plan.role)
                          << "): " << plan.critical_dependencies << " critical dependencies\n";
            }
        }

        if (failed) {
            return 1;
        }
        std::cout << "Verified " << image_count << " supported images and " << plan_count << " applicable plans.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Verifier failed unexpectedly: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Verifier failed unexpectedly.\n";
        return 1;
    }
}
