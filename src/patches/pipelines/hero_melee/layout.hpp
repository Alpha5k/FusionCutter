#pragma once

#include <FusionCutter/patching.hpp>

#include <cstdint>

namespace fusioncutter::patches::hero_melee_pipeline {

// Identifies the shared native boundaries used by hero melee policy and observation patches.
struct HeroMeleeLayout {
    NativeSite<16> update;
    NativeSite<16> set_network_state;
    NativeSite<16> enter_state;
    NativeSite<16> animator_state;
    NativeSite<16> input_queue_update;
    NativeSite<16> prediction_transition;
    std::uint32_t input_queue_constant_rva;
};

[[nodiscard]] const HeroMeleeLayout& layout_for(TargetLayout target) noexcept;
[[nodiscard]] std::array<std::byte, 16> input_queue_preimage(const ImageContext& image,
                                                             const HeroMeleeLayout& layout) noexcept;

} // namespace fusioncutter::patches::hero_melee_pipeline
