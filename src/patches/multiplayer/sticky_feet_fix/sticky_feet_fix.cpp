#include "sticky_feet_fix.hpp"

#include "layout.hpp"

namespace fusioncutter::patches::sticky_feet_fix {
namespace {

constexpr std::uint32_t kEnergyExhausted = 1;
constexpr float kProjectionGrace = 0.1F;

struct Vector3 {
    float x;
    float y;
    float z;
};

[[nodiscard]] float dot(const Vector3& left, const Vector3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

PatchInstanceSlot<StickyFeetFix> gPatch;

} // namespace

StickyFeetFix::StickyFeetFix(const TargetContext& target) noexcept
    : jump_using_energy_(target.image.function_at_rva<JumpUsingEnergy>(layout::kJumpUsingEnergyRva)),
      threshold_epsilon_(target.image.read_at_rva<float>(layout::kThresholdEpsilonRva)) {}

void StickyFeetFix::build_plan(PatchPlan& plan) {
    plan.redirect_call("Recover primary locked jump", layout::kPrimaryJumpCallRva,
                       BytePattern::exact(layout::kPrimaryJumpCall), &StickyFeetFix::jump_hook);
    plan.redirect_call("Recover secondary locked jump", layout::kSecondaryJumpCallRva,
                       BytePattern::exact(layout::kSecondaryJumpCall), &StickyFeetFix::jump_hook);
}

void StickyFeetFix::enable_runtime() noexcept {
    gPatch.publish(*this);
}

void StickyFeetFix::disable_runtime() noexcept {
    gPatch.clear(*this);
}

// Limits the fallback to the measured low-stamina classification boundary.
bool StickyFeetFix::should_recover_jump(void* soldier) const noexcept {
    const auto energy_flags = read_native_field<std::uint32_t>(soldier, layout::kEnergyFlagsOffset);
    const auto* soldier_class = read_native_field<const void*>(soldier, layout::kSoldierClassOffset);
    const auto normal_cost = read_native_field<float>(soldier_class, layout::kNormalJumpCostOffset);
    const auto sprint_cost = read_native_field<float>(soldier_class, layout::kSprintJumpCostOffset);
    const auto normal_speed = read_native_field<float>(soldier_class, layout::kNormalSpeedOffset);
    const auto velocity = read_native_field<Vector3>(soldier, layout::kVelocityOffset);
    const auto forward = read_native_field<Vector3>(soldier, layout::kForwardOffset);
    const auto projection = dot(velocity, forward);
    const auto threshold = normal_speed + *threshold_epsilon_;

    return (energy_flags & kEnergyExhausted) != 0 && normal_cost <= 0.0F && sprint_cost > normal_cost &&
           projection > threshold && projection <= threshold + kProjectionGrace;
}

// Calls the same Controllable transition reached after a successful native energy check.
bool StickyFeetFix::apply_jump_state(void* soldier) noexcept {
    auto* controllable = static_cast<std::byte*>(soldier) + layout::kControllableOffset;
    const auto vtable = read_native_field<void**>(controllable);
    const auto apply_jump = reinterpret_cast<JumpUsingEnergy>(vtable[layout::kApplyJumpVtableOffset / sizeof(void*)]);
    return apply_jump(controllable);
}

bool StickyFeetFix::jump(void* soldier) const noexcept {
    return should_recover_jump(soldier) ? apply_jump_state(soldier) : jump_using_energy_(soldier);
}

bool __fastcall StickyFeetFix::jump_hook(void* soldier, void*) noexcept {
    const auto* patch = gPatch.read();
    return patch != nullptr && patch->jump(soldier);
}

} // namespace fusioncutter::patches::sticky_feet_fix
