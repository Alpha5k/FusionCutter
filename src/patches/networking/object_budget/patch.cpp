#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "object_budget.hpp"

namespace fusioncutter::patches::object_budget {
namespace {

const PatchVariants kVariants{
    make_patch_variant<ObjectBudget, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Object Budget",
        .enabled = true,
        .configurable = true,
        .category = categories::Networking,
        .description = "Reserve update space for game events when the event queue is busy.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::object_budget
