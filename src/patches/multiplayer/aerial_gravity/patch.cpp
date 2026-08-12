#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "aerial_gravity.hpp"

namespace fusioncutter::patches::aerial_gravity {
namespace {

const PatchVariants kVariants{
    make_patch_variant<AerialGravity, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<AerialGravity, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<AerialGravity, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Aerial Gravity",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Keep hero aerial gravity consistent across simulation updates.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::aerial_gravity
