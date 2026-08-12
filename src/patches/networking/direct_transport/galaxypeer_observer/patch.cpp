#include "observer.hpp"

#include <FusionCutter/categories.hpp>

#include <array>

namespace fusioncutter::patches::galaxypeer_observer {
namespace {

constexpr std::array<PatchRelationship, 1> kDependsOn{"DirectTransport"};

const PatchVariants kVariants{
    make_patch_variant<GalaxyPeerObserver, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::GalaxyPeer,
                                                                                      ImageTiming::OneShotLate),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "GalaxyPeer Observer",
        .enabled = false,
        .configurable = false,
        .category = categories::Networking,
        .depends_on = kDependsOn,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::galaxypeer_observer
