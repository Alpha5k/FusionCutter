#pragma once

#include <FusionCutter/patch.hpp>

#include <atomic>

namespace fusioncutter::patches::map_hang_fix {

// Releases a map transition after its native network readiness gate remains blocked for 100 calls.
class MapHangFix final : public RuntimePatch, public Updatable {
  public:
    explicit MapHangFix(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;
    void update() noexcept override;

  private:
    const std::uint8_t* map_status_;
    std::atomic_uint32_t blocked_calls_{};

    static void observe_readiness(MidHookContext& context) noexcept;
};

} // namespace fusioncutter::patches::map_hang_fix
