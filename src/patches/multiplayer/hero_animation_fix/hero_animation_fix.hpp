#pragma once

#include "history.hpp"
#include "layout.hpp"
#include "../../pipelines/hero_melee/pipeline.hpp"

#include <FusionCutter/patch.hpp>

#include <array>
#include <cstdint>

namespace fusioncutter::patches::hero_animation_fix {

// Owns hero melee prediction history and supplies reconciliation policy to the shared pipeline.
class HeroAnimationFix final : public RuntimePatch {
  public:
    explicit HeroAnimationFix(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using GetJoystickIndex = int(__cdecl*)(int);

    struct PresentationBinding {
        HeroIdentity identity{};
        void* animator{};
    };

    [[nodiscard]] hero_melee_pipeline::UpdateDecision begin_update(void* weapon, float delta) noexcept;
    void finish_update(void* weapon, float delta, bool native_called, bool result) noexcept;
    [[nodiscard]] hero_melee_pipeline::NetworkStateDecision begin_network_state(void* weapon, int state,
                                                                                bool flag) noexcept;
    void finish_network_state(void* weapon, int state, bool flag, bool native_called) noexcept;
    void observe_enter_state(void* weapon, int state) noexcept;
    [[nodiscard]] bool suppress_animator_state(void* animator, std::uint32_t weapon_state, std::uint32_t active,
                                               std::uint32_t primary_animation,
                                               std::uint32_t secondary_animation) noexcept;
    void reconcile_input_queue(MidHookContext& context) noexcept;
    void suppress_authority_replay(MidHookContext& context) noexcept;

    // Captures the exact selected, alive melee identity required at every reconciliation boundary.
    [[nodiscard]] HeroIdentity capture_identity(void* weapon, bool& ready) const noexcept;
    [[nodiscard]] static void* selected_weapon(const HeroIdentity& identity) noexcept;
    [[nodiscard]] bool network_prediction_active() noexcept;
    void observe_prediction_transition(void* weapon, int state) noexcept;
    void clear_identity(const HeroIdentity& identity) noexcept;
    void clear_weapon(std::uintptr_t weapon) noexcept;
    void clear_tracking() noexcept;

    const HeroAnimationLayout& layout_;
    ImageContext image_;
    GetJoystickIndex get_joystick_index_{};
    const volatile std::int32_t* prediction_turn_{};
    const volatile std::int32_t* acknowledged_turn_{};
    const volatile std::uint8_t* is_local_turn_{};
    const volatile std::uint8_t* is_update_turn_{};
    const volatile std::uint8_t* network_enabled_{};
    const volatile std::uint8_t* network_client_active_{};
    std::uintptr_t prediction_resume_{};
    HeroHistory history_;
    std::array<PresentationBinding, kLocalPlayers> presentations_{};
    hero_melee_pipeline::PolicyCallbacks pipeline_callbacks_;
    bool prediction_was_active_{};
};

} // namespace fusioncutter::patches::hero_animation_fix
