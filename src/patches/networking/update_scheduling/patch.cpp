#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "update_scheduling.hpp"

namespace fusioncutter::patches::update_scheduling {
namespace {

const PatchVariants kVariants{
    make_patch_variant<UpdateScheduling, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Update Scheduling",
        .enabled = true,
        .configurable = true,
        .category = categories::Networking,
        .description = "Send server updates every turn while preserving object-creation ordering.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::update_scheduling
