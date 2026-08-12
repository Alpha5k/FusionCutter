#include "patch.hpp"

#include <FusionCutter/categories.hpp>

namespace fusioncutter::patches::distance_lag_fix {
namespace {

constexpr std::uint32_t kPlayerMoveLimitRva = 0x001D38B8;
constexpr std::uint8_t kStockPlayerMoveLimit = 5;
constexpr std::uint8_t kExpandedPlayerMoveLimit = 32;

// Expands the native distance-ranked movement sample list without changing its codec.
class DistanceLagFix final : public Patch {
  public:
    explicit DistanceLagFix(const TargetContext&) noexcept {}

    void build_plan(PatchPlan& plan) override {
        plan.checked_write("Expand player movement relay", kPlayerMoveLimitRva, kStockPlayerMoveLimit,
                           kExpandedPlayerMoveLimit);
    }
};

const PatchVariants kVariants{
    make_patch_variant<DistanceLagFix, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Distance Lag Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Networking,
        .description = "Relay input updates from the nearest 32 players to each client instead of only the nearest 5.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::distance_lag_fix
