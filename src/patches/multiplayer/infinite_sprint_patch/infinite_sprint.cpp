#include "infinite_sprint.hpp"

#include "layout.hpp"

namespace fusioncutter::patches::infinite_sprint_patch {
namespace {

constexpr std::uint32_t kEnergyExhausted = 1;
PatchInstanceSlot<InfiniteSprintPatch> gPatch;

} // namespace

InfiniteSprintPatch::InfiniteSprintPatch(const TargetContext& target) noexcept
    : roll_using_energy_(target.image.function_at_rva<RollUsingEnergy>(layout::kRollUsingEnergyRva)) {}

void InfiniteSprintPatch::build_plan(PatchPlan& plan) {
    plan.redirect_call("Repair failed sprint roll", layout::kSprintRollCallRva,
                       BytePattern::exact(layout::kSprintRollCall), &InfiniteSprintPatch::roll_hook);
}

void InfiniteSprintPatch::enable_runtime() noexcept {
    gPatch.publish(*this);
}

void InfiniteSprintPatch::disable_runtime() noexcept {
    gPatch.clear(*this);
}

// Uses the native Controllable transition instead of writing sprint flags directly.
void InfiniteSprintPatch::end_sprint(void* soldier) noexcept {
    auto* controllable = static_cast<std::byte*>(soldier) + layout::kControllableOffset;
    const auto vtable = read_native_field<void**>(controllable);
    const auto end = reinterpret_cast<void(__thiscall*)(void*)>(vtable[layout::kEndSprintVtableOffset / sizeof(void*)]);
    end(controllable);
}

bool InfiniteSprintPatch::roll(void* soldier) const noexcept {
    const auto rolled = roll_using_energy_(soldier);
    const auto energy_flags = read_native_field<std::uint32_t>(soldier, layout::kEnergyFlagsOffset);
    if (!rolled && (energy_flags & kEnergyExhausted) != 0) {
        end_sprint(soldier);
    }
    return rolled;
}

bool __fastcall InfiniteSprintPatch::roll_hook(void* soldier, void*) noexcept {
    const auto* patch = gPatch.read();
    return patch != nullptr && patch->roll(soldier);
}

} // namespace fusioncutter::patches::infinite_sprint_patch
