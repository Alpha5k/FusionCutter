#include "map_hang_fix.hpp"

#include "layout.hpp"

namespace fusioncutter::patches::map_hang_fix {
namespace {

constexpr std::uint8_t kMapIdle = 0;
constexpr std::uint32_t kBlockedCallLimit = 100;
PatchInstanceSlot<MapHangFix> gPatch;

} // namespace

MapHangFix::MapHangFix(const TargetContext& target) noexcept
    : map_status_(target.image.read_at_rva<std::uint8_t>(layout::kMapStatusRva)) {}

void MapHangFix::build_plan(PatchPlan& plan) {
    plan.mid_hook("Watch map-transition readiness", layout::kReadinessDecisionRva,
                  BytePattern::exact(layout::kReadinessDecision), &MapHangFix::observe_readiness);
}

void MapHangFix::enable_runtime() noexcept {
    gPatch.publish(*this);
}

void MapHangFix::disable_runtime() noexcept {
    gPatch.clear(*this);
}

// A new idle episode re-arms the watchdog without mutating map state from the loader thread.
void MapHangFix::update() noexcept {
    if (*map_status_ == kMapIdle) {
        blocked_calls_.store(0, std::memory_order_release);
    }
}

// Forces the native network-disabled/ready result only after the current transition stalls.
void MapHangFix::observe_readiness(MidHookContext& context) noexcept {
    auto* patch = gPatch.read();
    if (patch == nullptr) {
        return;
    }

    const auto calls = patch->blocked_calls_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (calls >= kBlockedCallLimit) {
        context.eax = 0;
    }
}

} // namespace fusioncutter::patches::map_hang_fix
