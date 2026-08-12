#pragma once

#include "layout.hpp"

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::aerial_gravity {

// Uses the simulation-turn timestep for melee aerial gravity on every supported role.
class AerialGravity final : public Patch {
  public:
    explicit AerialGravity(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;

  private:
    ImageContext image_;
    const AerialGravityLayout& layout_;
    float scaled_gravity_;
};

} // namespace fusioncutter::patches::aerial_gravity
