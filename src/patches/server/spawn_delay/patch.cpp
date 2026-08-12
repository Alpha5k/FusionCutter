#include "patch.hpp"

#include <FusionCutter/categories.hpp>

namespace fusioncutter::patches::spawn_delay {
namespace {

constexpr std::string_view kSpawnTimerVariable = "SPAWN_TIMER";
constexpr float kStockSpawnDelay = 15.0F;
constexpr std::uint32_t kSpawnDelayOperandRva = 0x0018D609;
constexpr std::uint32_t kStockSpawnDelayRva = 0x003B3228;

// Redirects the network respawn delay to the value supplied by the server manager.
class SpawnDelay final : public Patch {
  public:
    explicit SpawnDelay(const TargetContext& target) noexcept : image_(target.image) {
        const auto configured = read_environment_value<float>(kSpawnTimerVariable);
        if (configured.has_value() && configured->has_value() && **configured >= 0.0F) {
            spawn_delay_ = **configured;
        }
    }

    void build_plan(PatchPlan& plan) override {
        const auto stock_delay = static_cast<std::uint32_t>(image_.address_at_rva(kStockSpawnDelayRva));
        plan.checked_write("Use configured respawn delay", kSpawnDelayOperandRva, stock_delay,
                           PatchAddress::absolute(&spawn_delay_));
    }

  private:
    ImageContext image_;
    float spawn_delay_{kStockSpawnDelay};
};

const PatchVariants kVariants{
    make_patch_variant<SpawnDelay, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Spawn Delay",
        .enabled = true,
        .configurable = true,
        .category = categories::Server,
        .description = "Use the server's configured spawn delay value instead of the default 15.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::spawn_delay
