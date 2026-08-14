#include "pipeline.hpp"

#include <FusionCutter/categories.hpp>

namespace fusioncutter::patches::soldier_state_pipeline {
namespace {

const PatchVariants kVariants{
    make_patch_variant<SoldierStatePipeline, TargetLayout::SteamRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<SoldierStatePipeline, TargetLayout::GOGRetail, HostRole::Client>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Soldier State Pipeline",
        .enabled = false,
        .configurable = false,
        .category = categories::Diagnostics,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::soldier_state_pipeline
