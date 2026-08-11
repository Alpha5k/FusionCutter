#pragma once

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::dlc_mission_limit {

class DLCMissionLimit final : public Patch {
  public:
    explicit DLCMissionLimit(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;

  private:
    TargetLayout layout_;
    ImageContext image_;
};

} // namespace fusioncutter::patches::dlc_mission_limit
