#pragma once

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::gog_fixes {

// Applies the `/norender`, join-password, and Galaxy lobby metadata repairs for GOG dedicated servers.
class GogServerFixes final : public Patch {
  public:
    explicit GogServerFixes(const TargetContext& target) noexcept : image_(target.image) {}

    void build_plan(PatchPlan& plan) override;

  private:
    ImageContext image_;
};

} // namespace fusioncutter::patches::gog_fixes
