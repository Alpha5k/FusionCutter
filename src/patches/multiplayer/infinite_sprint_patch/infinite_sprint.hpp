#pragma once

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::infinite_sprint_patch {

// Restores the missing sprint cleanup after an exhausted roll attempt fails.
class InfiniteSprintPatch final : public RuntimePatch {
  public:
    explicit InfiniteSprintPatch(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using RollUsingEnergy = bool(__thiscall*)(void*);

    RollUsingEnergy roll_using_energy_;

    [[nodiscard]] bool roll(void* soldier) const noexcept;
    static void end_sprint(void* soldier) noexcept;
    [[nodiscard]] static bool __fastcall roll_hook(void* soldier, void*) noexcept;
};

} // namespace fusioncutter::patches::infinite_sprint_patch
