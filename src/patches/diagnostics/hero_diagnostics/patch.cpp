#include "patch.hpp"

#include "hero_diagnostics.hpp"

#include <FusionCutter/categories.hpp>

#include <array>

namespace fusioncutter::patches::hero_diagnostics {
namespace {

constexpr std::array kCaptureModes{
    ChoiceValue{"Standard", CaptureMode::Standard},
    ChoiceValue{"Combat", CaptureMode::Combat},
    ChoiceValue{"Full", CaptureMode::Full},
};

constexpr std::array<PatchRelationship, 2> kDependsOn{
    PatchRelationship{"HeroMeleePipeline", HostRole::Client},
    PatchRelationship{"SoldierStatePipeline", HostRole::Client},
};

const auto kSettings = SettingsDefinition::from(SettingsSchema<HeroDiagnosticsSettings>{
    .values =
        {
            choice("Capture", &HeroDiagnosticsSettings::capture, CaptureMode::Standard, kCaptureModes)
                .description("Controls how much hero melee detail is recorded."),
        },
});

const PatchVariants kVariants{
    make_patch_variant<HeroDiagnostics, TargetLayout::SteamRetail, HostRole::Client, HeroDiagnosticsSettings>(
        TargetImage::Game),
    make_patch_variant<HeroDiagnostics, TargetLayout::GOGRetail, HostRole::Client, HeroDiagnosticsSettings>(
        TargetImage::Game),
    make_patch_variant<HeroDiagnostics, TargetLayout::GOGRetail, HostRole::Server, HeroDiagnosticsSettings>(
        TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Hero Diagnostics",
        .enabled = false,
        .configurable = true,
        .category = categories::Diagnostics,
        .description = "Records hero melee state, movement, animation, and network evidence for troubleshooting.",
        .settings = kSettings,
        .depends_on = kDependsOn,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::hero_diagnostics
