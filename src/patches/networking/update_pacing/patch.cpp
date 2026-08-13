#include "update_pacing.hpp"

#include <FusionCutter/categories.hpp>

#include <array>

namespace fusioncutter::patches::update_pacing {
namespace {

constexpr std::array<PatchRelationship, 1> kDependsOn{"DirectTransport"};

const PatchVariants kVariants{
    make_patch_variant<UpdatePacing, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Update Pacing",
        .enabled = false,
        .configurable = true,
        .category = categories::Networking,
        .description =
            "Spaces Direct Transport server updates to reduce multiple updates arriving in the same client frame.",
        .depends_on = kDependsOn,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::update_pacing
