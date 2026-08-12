#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "update_recovery.hpp"

namespace fusioncutter::patches::update_recovery {
namespace {

const PatchVariants kVariants{
    make_patch_variant<LateUpdateRecovery, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<LateUpdateRecovery, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Late Update Recovery",
        .enabled = true,
        .configurable = true,
        .category = categories::Networking,
        .description = "Prevent game events from being skipped when several updates arrive together.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::update_recovery
