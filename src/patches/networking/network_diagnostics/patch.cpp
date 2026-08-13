#include "patch.hpp"

#include "network_diagnostics.hpp"

#include <FusionCutter/categories.hpp>

#include <array>

namespace fusioncutter::patches::network_diagnostics {
namespace {

constexpr std::array kCaptureModes{
    ChoiceValue{"Standard", CaptureMode::Standard},
    ChoiceValue{"Combat", CaptureMode::Combat},
    ChoiceValue{"Full", CaptureMode::Full},
};

constexpr std::array<PatchRelationship, 1> kDependsOn{"NetworkPipeline"};

const auto kSettings = SettingsDefinition::from(SettingsSchema<NetworkDiagnosticsSettings>{
    .values =
        {
            choice("Capture", &NetworkDiagnosticsSettings::capture, CaptureMode::Standard, kCaptureModes)
                .description("Controls how much network and gameplay detail is recorded."),
            setting("MaximumFileSizeMB", &NetworkDiagnosticsSettings::maximum_file_size_mb, std::uint32_t{512})
                .range(std::uint32_t{1}, std::uint32_t{65'535})
                .description("Stops the trace after it reaches this size."),
        },
});

const PatchVariants kVariants{
    make_patch_variant<NetworkDiagnostics, TargetLayout::GOGRetail, HostRole::Client, NetworkDiagnosticsSettings>(
        TargetImage::Game),
    make_patch_variant<NetworkDiagnostics, TargetLayout::SteamRetail, HostRole::Client, NetworkDiagnosticsSettings>(
        TargetImage::Game),
    make_patch_variant<NetworkDiagnostics, TargetLayout::GOGRetail, HostRole::Server, NetworkDiagnosticsSettings>(
        TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Network Diagnostics",
        .enabled = false,
        .configurable = true,
        .category = categories::Networking,
        .description = "Records multiplayer timing, delivery, prediction, and combat evidence for troubleshooting.",
        .settings = kSettings,
        .depends_on = kDependsOn,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::network_diagnostics
