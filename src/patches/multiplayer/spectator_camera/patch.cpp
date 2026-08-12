#include "patch.hpp"

#include "../../categories.hpp"
#include "spectator_camera.hpp"

namespace fusioncutter::patches::spectator_camera {
namespace {

const PatchVariants kVariants{
    make_patch_variant<SpectatorCameraSmoothing, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<SpectatorCameraSmoothing, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Spectator Camera Smoothing",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Smooth jerky camera movement while spectating other players.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::spectator_camera
