#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "hero_animation_fix.hpp"

namespace fusioncutter::patches::hero_animation_fix {
namespace {

const PatchVariants kVariants{
    make_patch_variant<HeroAnimationFix, TargetLayout::SteamRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<HeroAnimationFix, TargetLayout::GOGRetail, HostRole::Client>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Hero Animation Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description =
            "Prevent multiplayer hero melee animations from advancing twice or restarting after delayed updates.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::hero_animation_fix
