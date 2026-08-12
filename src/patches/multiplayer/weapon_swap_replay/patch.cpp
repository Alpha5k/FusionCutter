#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "weapon_swap.hpp"

namespace fusioncutter::patches::weapon_swap_replay {
namespace {

const PatchVariants kVariants{
    make_patch_variant<WeaponSwapReplayFix, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<WeaponSwapReplayFix, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Weapon Swap Replay Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Prevent weapon swaps, sounds, and visuals from repeating after network updates.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::weapon_swap_replay
