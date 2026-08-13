#pragma once

#include <FusionCutter/patching.hpp>

#include <array>
#include <cstdint>

namespace fusioncutter::patches::hero_animation_fix {

// Identifies the native melee, animator, and private input boundaries owned by the patch.
struct HeroAnimationHooks {
    NativeSite<16> update;
    NativeSite<16> set_network_state;
    NativeSite<16> enter_state;
    NativeSite<16> animator_state;
    NativeSite<16> input_queue_update;
    NativeSite<16> prediction_transition;
    NativeSite<32> input_queue_call;
    std::uint32_t input_queue_constant_rva;
    std::uint32_t prediction_resume_rva;
};

// Identifies the client prediction globals and their native provenance sites.
struct HeroAnimationState {
    std::uint32_t prediction_turn_rva;
    std::uint32_t acknowledged_turn_rva;
    std::uint32_t is_local_turn_rva;
    std::uint32_t is_update_turn_rva;
    std::uint32_t update_turn_rva;
    std::uint32_t client_host_turn_rva;
    std::uint32_t network_enabled_rva;
    std::uint32_t network_fallback_rva;
    std::uint32_t network_override_rva;
    std::uint32_t network_client_active_rva;
    NativeSite<32> local_turn_context;
    NativeSite<16> update_turn_context;
    NativeSite<12> acknowledged_turn_store;
    NativeSite<41> network_state_guard;
};

struct HeroAnimationLayout {
    NativeSite<13> joystick_lookup;
    HeroAnimationHooks hooks;
    HeroAnimationState state;
};

[[nodiscard]] const HeroAnimationLayout& layout_for(TargetLayout target) noexcept;
// Resolves the one relocated constant referenced by InputQueue::Update's prologue.
[[nodiscard]] std::array<std::byte, 16> input_queue_preimage(const ImageContext& image,
                                                             const HeroAnimationLayout& layout) noexcept;
// Proves the native global operands and private input call shape used outside installed hooks.
void add_layout_requirements(PatchPlan& plan, const ImageContext& image, const HeroAnimationLayout& layout);

} // namespace fusioncutter::patches::hero_animation_fix
