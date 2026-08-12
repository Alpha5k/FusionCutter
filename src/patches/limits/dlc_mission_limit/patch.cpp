#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "dlc_mission_limit.hpp"

namespace fusioncutter::patches::dlc_mission_limit {
namespace {

const PatchVariants kVariants{
    make_patch_variant<DLCMissionLimit, TargetLayout::SteamRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<DLCMissionLimit, TargetLayout::SteamRetail, HostRole::Server>(TargetImage::Game),
    make_patch_variant<DLCMissionLimit, TargetLayout::GOGRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<DLCMissionLimit, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::Game),
    make_patch_variant<DLCMissionLimit, TargetLayout::ModTools, HostRole::Client>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "DLC Mission Limit",
        .enabled = true,
        .configurable = true,
        .category = categories::Limits,
        .description = "Raise the DLC and add-on mission limit from 500 to 4096.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::dlc_mission_limit
