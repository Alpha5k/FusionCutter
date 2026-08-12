#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "crouch_bug_fix.hpp"

namespace fusioncutter::patches::crouch_bug_fix {
namespace {

const PatchVariants kVariants{
    make_patch_variant<CrouchBugFix, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<CrouchBugFix, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Crouch Bug Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Preserve remote players' crouch state between network updates.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::crouch_bug_fix
