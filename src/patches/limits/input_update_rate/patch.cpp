#include <FusionCutter/categories.hpp>

#include <FusionCutter/patch.hpp>

#include <cstdint>
#include <utility>

namespace fusioncutter::patches::input_update_rate {
namespace {

[[nodiscard]] constexpr std::uint32_t divisor_rva(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
    case TargetLayout::GOGRetail:
        return 0x0012D4C2;
    case TargetLayout::ModTools:
        return 0x00049B5B;
    case TargetLayout::Aspyr:
        std::unreachable();
    }
    std::unreachable();
}

class InputUpdateRate final : public Patch {
  public:
    explicit InputUpdateRate(const TargetContext& target) noexcept : divisor_rva_(divisor_rva(target.layout)) {}

    void build_plan(PatchPlan& plan) override {
        constexpr auto kPushImmediate = byte_array<0x6A>();
        constexpr std::uint8_t kThirtyHertz = 0x1E;
        constexpr std::uint8_t kOneHundredTwentyHertz = 0x78;

        // The second fixed-rate timer gates keyboard, joystick, and voice sampling; the simulation timer is separate.
        plan.require_bytes("Verify input timer instruction", divisor_rva_ - kPushImmediate.size(),
                           BytePattern::exact(kPushImmediate));
        plan.checked_write("Raise input update rate", divisor_rva_, kThirtyHertz, kOneHundredTwentyHertz);
    }

  private:
    std::uint32_t divisor_rva_{};
};

const PatchVariants kVariants{
    make_patch_variant<InputUpdateRate, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<InputUpdateRate, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<InputUpdateRate, TargetLayout::ModTools>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Input Update Rate",
        .enabled = true,
        .configurable = true,
        .category = categories::Limits,
        .description = "Raise input and voice sampling from 30 Hz to 120 Hz.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::input_update_rate
