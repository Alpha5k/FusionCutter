#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "map_hang_fix.hpp"

namespace fusioncutter::patches::map_hang_fix {
namespace {

const PatchVariants kVariants{
    make_patch_variant<MapHangFix, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Map Hang Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Server,
        .description = "Recover dedicated servers that become stuck during map changes.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::map_hang_fix
