#include "aerial_gravity.hpp"

namespace fusioncutter::patches::aerial_gravity {
namespace {

constexpr float kStockAerialGravity = 18.0F;
constexpr float kAerialGravityScale = 0.80F;

} // namespace

AerialGravity::AerialGravity(const TargetContext& target) noexcept
    : image_(target.image), layout_(layout_for(target.layout)),
      scaled_gravity_(kStockAerialGravity * kAerialGravityScale) {}

void AerialGravity::build_plan(PatchPlan& plan) {
    const auto stock_gravity = static_cast<std::uint32_t>(image_.address_at_rva(layout_.stock_gravity_rva));
    const auto outer_time = static_cast<std::uint32_t>(image_.address_at_rva(layout_.outer_time_rva));

    // The original operation runs once per simulation turn but reads the outer-loop elapsed time.
    plan.checked_write("Scale melee aerial gravity", layout_.gravity_operand_rva, stock_gravity,
                       PatchAddress::absolute(&scaled_gravity_));
    plan.checked_write("Use simulation-turn time", layout_.turn_time_operand_rva, outer_time,
                       PatchAddress::image_rva(layout_.turn_time_rva));
}

} // namespace fusioncutter::patches::aerial_gravity
