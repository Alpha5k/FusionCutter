#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::crouch_bug_fix {

class CrouchBugFix final : public RuntimePatch {
  public:
    explicit CrouchBugFix(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using GetJoystickIndex = int(__cdecl*)(int player_handle) noexcept;

    [[nodiscard]] std::uint32_t resolve_input(const void* crouch_trigger, std::uint32_t input_down) const noexcept;
    static void sample_crouch_input(MidHookContext& context) noexcept;

    TargetLayout layout_;
    GetJoystickIndex get_joystick_index_{};

    inline static PatchInstanceSlot<CrouchBugFix> active_;
};

} // namespace fusioncutter::patches::crouch_bug_fix
