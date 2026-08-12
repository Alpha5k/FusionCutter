#include "update_scheduling.hpp"

#include "layout.hpp"

namespace fusioncutter::patches::update_scheduling {
namespace {

constexpr std::size_t kPlayerStateStride = 0x208;
constexpr std::size_t kPendingMapOffset = 0x4;
constexpr std::size_t kPendingTurnOffset = 0x104;
constexpr std::size_t kSmoothedRttOffset = 0x1C4;
constexpr std::size_t kRttVariationOffset = 0x1C8;
constexpr float kRetryFloorSeconds = 0.1F;

PatchInstanceSlot<UpdateScheduling> gPatch;

} // namespace

UpdateScheduling::UpdateScheduling(const TargetContext& target) noexcept
    : image_(target.image),
      allocate_object_map_(image_.function_at_rva<AllocateObjectMap>(layout::kAllocateObjectMapRva)),
      free_object_map_(image_.function_at_rva<FreeObjectMap>(layout::kFreeObjectMapRva)) {}

void UpdateScheduling::build_plan(PatchPlan& plan) {
    plan.nop("Remove dedicated presentation delay", layout::kDedicatedPresentCallRva,
             BytePattern::exact(layout::kDedicatedPresentCall));
    plan.nop("Visit every client", layout::kHalfClientLimiterRva, BytePattern::exact(layout::kHalfClientLimiter));

    // These three hooks preserve the native object-map transaction while shortening ordinary update eligibility.
    plan.mid_hook("Guard acknowledgement slots", layout::kSentSlotTimeCallRva,
                  BytePattern::exact(layout::kSentSlotTimeCall), &UpdateScheduling::guard_sent_slot);
    plan.mid_hook("Record emitted object creates", layout::kCreateMarkerRva, BytePattern::exact(layout::kCreateMarker),
                  &UpdateScheduling::capture_create);
    plan.mid_hook("Fence pending object creates", layout::kCreateFenceGateRva,
                  BytePattern::exact(layout::kCreateFenceGate), &UpdateScheduling::gate_destination);
    plan.checked_write("Schedule the next server turn", layout::kNextUpdateTurnRva,
                       BytePattern::exact(layout::kStockNextUpdateTurn), layout::kNextServerTurn);
}

void UpdateScheduling::enable_runtime() noexcept {
    gPatch.publish(*this);
}

void UpdateScheduling::disable_runtime() noexcept {
    gPatch.clear(*this);
    fences_.fill({});
}

std::byte* UpdateScheduling::player_state(std::size_t player) const noexcept {
    const auto address = image_.address_at_rva(layout::kPlayerStatesRva + player * kPlayerStateStride);
    return reinterpret_cast<std::byte*>(address);
}

// Snapshots native map identity only when WriteObjects reports that it emitted CREATE state.
void UpdateScheduling::record_create(std::uint32_t player) noexcept {
    if (player >= kMaximumPlayers) {
        return;
    }

    auto* state = player_state(player);
    auto* pending_map = read_native_field<void*>(state, kPendingMapOffset);
    if (pending_map == nullptr) {
        fences_[player] = {};
        return;
    }

    fences_[player] = {
        .pending_map = pending_map,
        .pending_turn = read_native_field<std::int32_t>(pending_map, kPendingTurnOffset),
        .sent_time = *image_.read_at_rva<float>(layout::kNetworkTimeRva),
    };
}

// Blocks ordinary updates until ACK/NACK changes map identity, then uses the native NACK reset on timeout.
bool UpdateScheduling::fence_blocks(std::uint32_t player) noexcept {
    if (player >= kMaximumPlayers) {
        return false;
    }

    auto& fence = fences_[player];
    if (fence.pending_map == nullptr) {
        return false;
    }

    auto* state = player_state(player);
    auto* live_map = read_native_field<void*>(state, kPendingMapOffset);
    if (live_map != fence.pending_map ||
        read_native_field<std::int32_t>(live_map, kPendingTurnOffset) != fence.pending_turn) {
        fence = {};
        return false;
    }

    auto retry =
        read_native_field<float>(state, kSmoothedRttOffset) + read_native_field<float>(state, kRttVariationOffset);
    if (!(retry >= kRetryFloorSeconds)) {
        retry = kRetryFloorSeconds;
    }

    const auto elapsed = *image_.read_at_rva<float>(layout::kNetworkTimeRva) - fence.sent_time;
    if (!(elapsed > retry)) {
        return true;
    }

    free_object_map_(live_map);
    write_native_field(state, kPendingMapOffset, allocate_object_map_());
    fence = {};
    return false;
}

// SendUpdate2 owns two acknowledgement slots; index two means neither slot is available.
void UpdateScheduling::guard_sent_slot(MidHookContext& context) noexcept {
    const auto slot = read_native_field<std::int32_t>(reinterpret_cast<void*>(context.ebp - sizeof(std::int32_t)));
    if (slot >= 2) {
        if (const auto* patch = gPatch.read(); patch != nullptr) {
            context.eip = patch->image_.address_at_rva(layout::kSentSlotSkipRva);
        }
    }
}

void UpdateScheduling::capture_create(MidHookContext& context) noexcept {
    auto* patch = gPatch.read();
    if (patch == nullptr) {
        return;
    }

    const auto emitted_create = read_native_field<std::uint8_t>(reinterpret_cast<void*>(context.ebp - 0x11));
    if (emitted_create != 0) {
        const auto player = *patch->image_.read_at_rva<std::uint32_t>(layout::kCurrentDestinationRva);
        patch->record_create(player);
    }
}

// Replaces the interval-only send-window branch with the CREATE fence while retaining the earlier pipe-full gate.
void UpdateScheduling::gate_destination(MidHookContext& context) noexcept {
    auto* patch = gPatch.read();
    if (patch == nullptr) {
        return;
    }

    const auto player = read_native_field<std::uint32_t>(reinterpret_cast<void*>(context.ebp - 0x10));
    const auto resume = patch->fence_blocks(player) ? layout::kSkipDestinationRva : layout::kSendDestinationRva;
    context.eip = patch->image_.address_at_rva(resume);
}

} // namespace fusioncutter::patches::update_scheduling
