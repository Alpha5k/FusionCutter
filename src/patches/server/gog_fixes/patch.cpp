#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "gog_fixes.hpp"

namespace fusioncutter::patches::gog_fixes {
namespace {

const PatchVariants kVariants{
    make_patch_variant<GogServerFixes, TargetLayout::GOGRetail, HostRole::Server>(
        TargetImage::Game, StartupFailurePolicy::StartupRequired),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "GOG Server Fixes",
        .enabled = true,
        .configurable = false,
        .category = categories::Server,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::gog_fixes
