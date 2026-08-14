#include "pipeline.hpp"

#include <FusionCutter/categories.hpp>

namespace fusioncutter::patches::network_pipeline {
namespace {

const PatchVariants kVariants{
    make_patch_variant<NetworkPipeline, TargetLayout::GOGRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<NetworkPipeline, TargetLayout::SteamRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<NetworkPipeline, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Network Pipeline",
        .enabled = false,
        .configurable = false,
        .category = categories::Networking,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::network_pipeline
