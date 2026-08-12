#pragma once

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::object_budget {

// Reduces recurring-object space only when ordinary game events need room in the same update.
class ObjectBudget final : public RuntimePatch {
  public:
    explicit ObjectBudget(const TargetContext& target) noexcept : image_(target.image) {}

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    ImageContext image_;

    [[nodiscard]] std::int32_t scaled_object_budget() const noexcept;
    static void apply_event_reserve(MidHookContext& context) noexcept;
};

} // namespace fusioncutter::patches::object_budget
