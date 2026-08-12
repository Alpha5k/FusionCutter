#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "infinite_sprint.hpp"

namespace fusioncutter::patches::infinite_sprint_patch {
namespace {

const PatchVariants kVariants{
    make_patch_variant<InfiniteSprintPatch, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Infinite Sprint Patch",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Patch the infinite sprint exploit caused by a well-timed roll at 0 stamina.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::infinite_sprint_patch
