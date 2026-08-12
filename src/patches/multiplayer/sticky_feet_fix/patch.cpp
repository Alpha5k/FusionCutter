#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "sticky_feet_fix.hpp"

namespace fusioncutter::patches::sticky_feet_fix {
namespace {

const PatchVariants kVariants{
    make_patch_variant<StickyFeetFix, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Sticky Feet Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Prevent valid low-stamina jumps from being rejected near sprint speed.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::sticky_feet_fix
