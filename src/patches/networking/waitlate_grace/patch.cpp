#include "patch.hpp"

#include <FusionCutter/categories.hpp>

namespace fusioncutter::patches::waitlate_grace {
namespace {

constexpr std::uint32_t kWaitlateGraceRva = 0x001BAAD0;
constexpr std::uint32_t kStockGraceTurns = 3;
constexpr std::uint32_t kGraceTurns = 1;

// Reduces only the `/waitlate` authority delay while leaving `/nowaitlate` unchanged.
class WaitlateGrace final : public Patch {
  public:
    explicit WaitlateGrace(const TargetContext&) noexcept {}

    void build_plan(PatchPlan& plan) override {
        plan.checked_write("Set /waitlate grace", kWaitlateGraceRva, kStockGraceTurns, kGraceTurns);
    }
};

const PatchVariants kVariants{
    make_patch_variant<WaitlateGrace, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Waitlate Grace",
        .enabled = true,
        .configurable = true,
        .category = categories::Networking,
        .description = "Change /waitlate grace value from 3 turns to 1.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::waitlate_grace
