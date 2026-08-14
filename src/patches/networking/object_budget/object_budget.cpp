#include "object_budget.hpp"

#include "layout.hpp"
#include "../../diagnostics/network_diagnostics/observations.hpp"

#include <algorithm>

namespace fusioncutter::patches::object_budget {
namespace {

constexpr std::uint32_t kMaxPlayers = 64;
constexpr std::uint32_t kPlayerStateStride = 0x208;
constexpr std::uint32_t kEventRingMask = 0x7F;
constexpr std::uint32_t kFreeEvents = 3;
constexpr std::int32_t kStockObjectScale = 800;
constexpr std::int32_t kReservePerEvent = 32;
constexpr std::int32_t kMaximumReserve = 200;

PatchInstanceSlot<ObjectBudget> gPatch;

} // namespace

void ObjectBudget::build_plan(PatchPlan& plan) {
    // Hook after the stock multiply so the native signed rounding and division remain intact.
    plan.mid_hook("Reserve congested event space", layout::kBudgetRoundingRva,
                  BytePattern::exact(layout::kBudgetRounding), &ObjectBudget::apply_event_reserve);
}

void ObjectBudget::enable_runtime() noexcept {
    gPatch.publish(*this);
}

void ObjectBudget::disable_runtime() noexcept {
    gPatch.clear(*this);
}

// Converts pending ordinary-event pressure into at most 200 scale units of reserved packet space.
std::int32_t ObjectBudget::scaled_object_budget() const noexcept {
    const auto destination = *image_.read_at_rva<std::uint32_t>(layout::kCurrentDestinationRva);
    auto object_scale = kStockObjectScale;
    std::uint32_t pending{};

    if (destination < kMaxPlayers) {
        const auto cursor_rva = layout::kDestinationEventCursorRva + destination * kPlayerStateStride;
        const auto cursor = *image_.read_at_rva<std::uint32_t>(cursor_rva) & kEventRingMask;
        const auto head = *image_.read_at_rva<std::uint32_t>(layout::kEventHeadRva) & kEventRingMask;
        pending = (head - cursor) & kEventRingMask;
        const auto pressured = pending > kFreeEvents ? pending - kFreeEvents : 0;
        const auto reserve = std::min(static_cast<std::int32_t>(pressured) * kReservePerEvent, kMaximumReserve);
        object_scale -= reserve;
    }

    network_diagnostics::observe_object_budget(destination, pending, object_scale);

    const auto update_size = *image_.read_at_rva<std::int32_t>(layout::kNetUpdateSizeRva);
    return static_cast<std::int32_t>(static_cast<std::int64_t>(update_size) * object_scale);
}

void ObjectBudget::apply_event_reserve(MidHookContext& context) noexcept {
    if (const auto* patch = gPatch.read(); patch != nullptr) {
        context.eax = static_cast<std::uint32_t>(patch->scaled_object_budget());
    }
}

} // namespace fusioncutter::patches::object_budget
