#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "door_corpse_fix.hpp"

namespace fusioncutter::patches::door_corpse_fix {
namespace {

const PatchVariants kVariants{
    make_patch_variant<DoorCorpseFix, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<DoorCorpseFix, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Door Corpse Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Prevent dead soldiers from blocking networked doors.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::door_corpse_fix
