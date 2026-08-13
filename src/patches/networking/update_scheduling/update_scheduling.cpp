#include "update_scheduling.hpp"

#include "layout.hpp"
#include "../network_diagnostics/observations.hpp"

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
    // bf2server_patch_netupdate: remove the dedicated render/present call that throttles the host loop.
    plan.nop("Remove dedicated server render call", layout::kDedicatedPresentCallRva,
             BytePattern::exact(layout::kDedicatedPresentCall));

    // bf2server_patch_netupdate: remove the /2 limiter so each host pass may visit every client.
    plan.nop("Visit all clients every host update", layout::kHalfClientLimiterRva,
             BytePattern::exact(layout::kHalfClientLimiter));

    // bf2_su2_slotfix_cc: keep SentUpdate inside its two real acknowledgement slots.
    plan.mid_hook("Protect SentUpdate acknowledgement slots", layout::kSentSlotTimeCallRva,
                  BytePattern::exact(layout::kSentSlotTimeCall), &UpdateScheduling::guard_sent_slot);

    // bf2_create_fence_cc: arm a fence only when WriteObjects emits CREATE state.
    plan.mid_hook("Track emitted object CREATE records", layout::kCreateMarkerRva,
                  BytePattern::exact(layout::kCreateMarker), &UpdateScheduling::capture_create);

    // bf2_create_fence_gate_cc: withhold ordinary updates until ACK/NACK settles the CREATE map.
    plan.mid_hook("Wait for object CREATE acknowledgement", layout::kCreateFenceGateRva,
                  BytePattern::exact(layout::kCreateFenceGate), &UpdateScheduling::gate_destination);

    // bf2server_patch_send_scheduling: make an unfenced destination eligible again next turn.
    plan.checked_write("Schedule an update every turn", layout::kNextUpdateTurnRva,
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
    network_diagnostics::observe_create_fence(player, fences_[player].pending_turn);
}

bool UpdateScheduling::fence_blocks(std::uint32_t player, bool& timed_out) noexcept {
    timed_out = false;
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

    // This is the native NACK reset path, which regenerates CREATE state on the next eligible update.
    free_object_map_(live_map);
    write_native_field(state, kPendingMapOffset, allocate_object_map_());
    fence = {};
    timed_out = true;
    return false;
}

void UpdateScheduling::guard_sent_slot(MidHookContext& context) noexcept {
    const auto slot = read_native_field<std::int32_t>(reinterpret_cast<void*>(context.ebp - sizeof(std::int32_t)));
    network_diagnostics::observe_ack_slot(slot, slot < 2);
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

void UpdateScheduling::gate_destination(MidHookContext& context) noexcept {
    auto* patch = gPatch.read();
    if (patch == nullptr) {
        return;
    }

    const auto player = read_native_field<std::uint32_t>(reinterpret_cast<void*>(context.ebp - 0x10));
    bool timed_out{};
    const auto blocked = patch->fence_blocks(player, timed_out);
    network_diagnostics::observe_destination_gate(player, blocked, timed_out);
    const auto resume = blocked ? layout::kSkipDestinationRva : layout::kSendDestinationRva;
    context.eip = patch->image_.address_at_rva(resume);
}

} // namespace fusioncutter::patches::update_scheduling
