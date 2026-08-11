#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::unlock_frame_rate {

struct UnlockFrameRateSettings {
    std::uint32_t max_frame_rate;
};

// Replaces the fixed frame limiter with a configured cap or fully uncapped timing.
class UnlockFrameRate final : public Patch {
  public:
    UnlockFrameRate(UnlockFrameRateSettings settings, const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;

  private:
    std::uint32_t max_frame_rate_{};
    TargetLayout layout_{};
};

} // namespace fusioncutter::patches::unlock_frame_rate
