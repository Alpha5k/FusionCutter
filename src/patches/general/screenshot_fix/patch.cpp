#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "screenshot_fix.hpp"

namespace fusioncutter::patches::screenshot_fix {
namespace {

const PatchVariants kVariants{
    make_patch_variant<ScreenshotFix, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<ScreenshotFix, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Screenshot Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::GeneralFixes,
        .description = "Prevent Print Screen from crashing the game and save screenshots as TGA files.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::screenshot_fix
