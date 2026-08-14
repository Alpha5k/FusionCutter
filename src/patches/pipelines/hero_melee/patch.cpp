#include "pipeline.hpp"

#include <FusionCutter/categories.hpp>

namespace fusioncutter::patches::hero_melee_pipeline {
namespace {

const PatchVariants kVariants{
    make_patch_variant<HeroMeleePipeline, TargetLayout::SteamRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<HeroMeleePipeline, TargetLayout::GOGRetail, HostRole::Client>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Hero Melee Pipeline",
        .enabled = false,
        .configurable = false,
        .category = categories::Multiplayer,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::hero_melee_pipeline
