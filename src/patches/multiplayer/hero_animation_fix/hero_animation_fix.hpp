#pragma once

#include "history.hpp"
#include "layout.hpp"

#include <FusionCutter/patch.hpp>

#include <array>
#include <cstdint>

namespace fusioncutter::patches::hero_animation_fix {

// Owns hero melee prediction history and the native boundaries that reconcile delayed authority.
class HeroAnimationFix final : public RuntimePatch {
  public:
    explicit HeroAnimationFix(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using UpdateFunction = bool(__thiscall*)(void*, float);
    using SetNetworkStateFunction = void(__thiscall*)(void*, int, bool);
    using EnterStateFunction = void(__thiscall*)(void*, int);
    using AnimatorStateFunction = void(__thiscall*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                    std::uint32_t);
    using GetJoystickIndex = int(__cdecl*)(int);

    struct PresentationBinding {
        HeroIdentity identity{};
        void* animator{};
    };

    [[nodiscard]] bool update_weapon(void* weapon, float delta) noexcept;
    void apply_network_state(void* weapon, int state, bool flag) noexcept;
    void enter_state(void* weapon, int state) noexcept;
    [[nodiscard]] bool suppress_animator_state(void* animator, std::uint32_t weapon_state, std::uint32_t active,
                                               std::uint32_t primary_animation,
                                               std::uint32_t secondary_animation) noexcept;

    // Captures the exact selected, alive melee identity required at every reconciliation boundary.
    [[nodiscard]] HeroIdentity capture_identity(void* weapon, bool& ready) const noexcept;
    [[nodiscard]] static void* selected_weapon(const HeroIdentity& identity) noexcept;
    [[nodiscard]] bool network_prediction_active() noexcept;
    void observe_prediction_transition(void* weapon, int state) noexcept;
    void clear_identity(const HeroIdentity& identity) noexcept;
    void clear_weapon(std::uintptr_t weapon) noexcept;
    void clear_tracking() noexcept;

    static bool __fastcall hook_update(void* weapon, void*, float delta) noexcept;
    static void __fastcall hook_set_network_state(void* weapon, void*, int state, bool flag) noexcept;
    static void __fastcall hook_enter_state(void* weapon, void*, int state) noexcept;
    static void __fastcall hook_animator_state(void* animator, void*, std::uint32_t weapon_state, std::uint32_t active,
                                               std::uint32_t primary_animation, std::uint32_t secondary_animation,
                                               std::uint32_t blend) noexcept;
    // Reconciles omitted remote button levels without detouring InputQueue::Update's private XMM2 ABI.
    static void reconcile_input_queue(MidHookContext& context) noexcept;
    // Skips only the exact prediction ExitState/EnterState pair already performed by authority.
    static void suppress_authority_replay(MidHookContext& context) noexcept;

    inline static PatchInstanceSlot<HeroAnimationFix> active_;
    inline static OriginalFunction<UpdateFunction> original_update_;
    inline static OriginalFunction<SetNetworkStateFunction> original_set_network_state_;
    inline static OriginalFunction<EnterStateFunction> original_enter_state_;
    inline static OriginalFunction<AnimatorStateFunction> original_animator_state_;

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
    bool prediction_was_active_{};
};

} // namespace fusioncutter::patches::hero_animation_fix
