#pragma once

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::sticky_feet_fix {

// Recovers valid exhausted jumps misclassified just above the normal-speed boundary.
class StickyFeetFix final : public RuntimePatch {
  public:
    explicit StickyFeetFix(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using JumpUsingEnergy = bool(__thiscall*)(void*);

    JumpUsingEnergy jump_using_energy_;
    const float* threshold_epsilon_;

    [[nodiscard]] bool jump(void* soldier) const noexcept;
    [[nodiscard]] bool should_recover_jump(void* soldier) const noexcept;
    [[nodiscard]] static bool apply_jump_state(void* soldier) noexcept;
    [[nodiscard]] static bool __fastcall jump_hook(void* soldier, void*) noexcept;
};

} // namespace fusioncutter::patches::sticky_feet_fix
