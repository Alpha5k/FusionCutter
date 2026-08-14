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

constexpr std::array<PatchRelationship, 2> kDependsOn{
    PatchRelationship{"NetworkPipeline"},
    PatchRelationship{"SoldierStatePipeline", HostRole::Client},
};

const auto kSettings = SettingsDefinition::from(SettingsSchema<NetworkDiagnosticsSettings>{
    .values =
        {
            choice("Capture", &NetworkDiagnosticsSettings::capture, CaptureMode::Standard, kCaptureModes)
                .description("Controls how much network and gameplay detail is recorded."),
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
        .category = categories::Diagnostics,
        .description = "Records multiplayer timing, delivery, prediction, and combat evidence for troubleshooting.",
        .settings = kSettings,
        .depends_on = kDependsOn,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::network_diagnostics
