#pragma once

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::reload_duration {

struct ReloadDurationTargetData;

// Repairs reload duration metadata omitted by authoritative Weapon::Read records for local players.
class ReloadDurationFix final : public RuntimePatch {
  public:
    explicit ReloadDurationFix(const TargetContext& target) noexcept;

    // Validates the Weapon::Read boundary and installs the post-decode repair callback.
    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using GetJoystickIndex = int(__cdecl*)(int player_handle) noexcept;

    // Restores the class reload time only for a locally controlled weapon actively reloading.
    void repair_duration(void* weapon) const noexcept;
    static void inspect_decoded_weapon(MidHookContext& context) noexcept;

    const ReloadDurationTargetData& target_;
    GetJoystickIndex get_joystick_index_{};

    inline static PatchInstanceSlot<ReloadDurationFix> active_;
};

} // namespace fusioncutter::patches::reload_duration
