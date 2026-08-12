#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "spawn_jump_fix.hpp"

namespace fusioncutter::patches::spawn_jump_fix {
namespace {

const PatchVariants kVariants{
    make_patch_variant<SpawnJumpFix, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Spawn Jump Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Server,
        .description = "Prevent players from spawning immediately when warmup timer ends.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::spawn_jump_fix
