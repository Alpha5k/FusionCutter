#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "reload_duration.hpp"

namespace fusioncutter::patches::reload_duration {
namespace {

const PatchVariants kVariants{
    make_patch_variant<ReloadDurationFix, TargetLayout::SteamRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<ReloadDurationFix, TargetLayout::GOGRetail, HostRole::Client>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Reload Duration Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Prevent weapon reload sounds from duplicating online.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::reload_duration
