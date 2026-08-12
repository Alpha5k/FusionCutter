#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "aspyr/rcon.hpp"
#include "gog/rcon.hpp"

namespace fusioncutter::patches::rcon_server {
namespace {

const PatchVariants kVariants{
    make_patch_variant<GogRcon, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::Game,
                                                                           StartupFailurePolicy::StartupRequired),
    make_patch_variant<AspyrRcon, TargetLayout::Aspyr, HostRole::Server>(TargetImage::Game,
                                                                         StartupFailurePolicy::StartupRequired),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "RCON Server",
        .enabled = true,
        .configurable = false,
        .category = categories::Server,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::rcon_server
