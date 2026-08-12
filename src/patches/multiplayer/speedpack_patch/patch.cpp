#include "patch.hpp"

#include <FusionCutter/categories.hpp>

#include <algorithm>
#include <cmath>

namespace fusioncutter::patches::speedpack_patch {
namespace {

constexpr std::uint32_t kWeaponDispenserFireRva = 0x00284C90;
constexpr auto kWeaponDispenserFirePrologue = byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0>();
constexpr std::size_t kDispenserStrengthOffset = 0x114;

// Restores the WeaponDispenser charge field to its network-defined [0,1] domain before authoritative firing.
void clamp_dispenser_strength(MidHookContext& context) noexcept {
    auto* weapon = reinterpret_cast<void*>(context.ecx);
    const auto strength = read_native_field<float>(weapon, kDispenserStrengthOffset);
    const auto clamped = std::isfinite(strength) ? std::clamp(strength, 0.0F, 1.0F) : 0.0F;
    write_native_field(weapon, kDispenserStrengthOffset, clamped);
}

// Clamps invalid dispenser charge without changing legitimate variable-strength throws.
class SpeedpackPatch final : public Patch {
  public:
    explicit SpeedpackPatch(const TargetContext&) noexcept {}

    void build_plan(PatchPlan& plan) override {
        plan.mid_hook("Clamp dispenser throw strength", kWeaponDispenserFireRva,
                      BytePattern::exact(kWeaponDispenserFirePrologue), &clamp_dispenser_strength);
    }
};

const PatchVariants kVariants{
    make_patch_variant<SpeedpackPatch, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Speedpack Patch",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Prevent the speedpack exploit from launching detpacks above their intended speed.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::speedpack_patch
