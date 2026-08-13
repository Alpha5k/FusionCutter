#include "../observers.hpp"

#include "codec.hpp"
#include "layout.hpp"
#include "../network_diagnostics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fusioncutter::patches::network_diagnostics {
namespace {

using WriteMove = void(__cdecl*)(void*, const void*, const void*) noexcept;
using WriteMiniMove = void(__cdecl*)(void*, const void*) noexcept;
using PacketFunction = void(__cdecl*)(void*) noexcept;
using ReceiveGroup = bool(__fastcall*)(void*, int) noexcept;
using SoldierReadObject = void(__fastcall*)(void*, void*, void*, void*) noexcept;
using SetRenderMatrix = void(__fastcall*)(void*, void*, const float*) noexcept;

struct PoseSampler {
    trace::ClientPoseRecord record{};
    std::uint32_t observed{};
    std::uint32_t previous_count{};
    std::uint32_t selected{};
    std::uint64_t sequence{};
    bool captured{};
};

OriginalFunction<WriteMove> gWriteMove;
OriginalFunction<WriteMiniMove> gWriteMiniMove;
OriginalFunction<PacketFunction> gReadPlayerMoves;
OriginalFunction<PacketFunction> gReadNetEvent;
OriginalFunction<PacketFunction> gReceiveReliableEvents;
OriginalFunction<ReceiveGroup> gReceiveGroup;
OriginalFunction<SoldierReadObject> gSoldierReadObject;
OriginalFunction<SetRenderMatrix> gSetRenderMatrix;
const client::ClientLayout* gLayout{};
ImageContext gImage{};
thread_local PoseSampler gAuthoritySampler;
thread_local PoseSampler gPresentationSampler;

[[nodiscard]] std::uint32_t pointer_id(const void* pointer) noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

template <typename Value> [[nodiscard]] Value read_global(std::uint32_t rva, Value fallback = {}) noexcept {
    const auto* value = gImage.read_at_rva<Value>(rva);
    return value == nullptr ? fallback : *value;
}

struct MoveIdentity {
    std::int32_t player{-1};
    std::int32_t turn_slot{-1};
};

// Identifies a local move by its fixed slot in the client's 32-turn history ring.
[[nodiscard]] MoveIdentity identify_move(const void* move) noexcept {
    constexpr std::size_t kTurnCount = 32;
    constexpr std::size_t kTurnStride = 0x30;
    constexpr std::size_t kPlayerStride = 0x18;
    if (move == nullptr || gLayout == nullptr) {
        return {};
    }

    const auto ring = gImage.address_at_rva(gLayout->move_ring_rva, kTurnCount * kTurnStride);
    const auto address = reinterpret_cast<std::uintptr_t>(move);
    if (ring == 0 || address < ring || address >= ring + kTurnCount * kTurnStride) {
        return {};
    }

    const auto offset = address - ring;
    const auto player_offset = offset % kTurnStride;
    if (player_offset % kPlayerStride != 0) {
        return {};
    }
    return {
        .player = static_cast<std::int32_t>(player_offset / kPlayerStride),
        .turn_slot = static_cast<std::int32_t>(offset / kTurnStride),
    };
}

void copy_move(trace::MoveCodecRecord& record, const void* move) noexcept {
    if (move == nullptr) {
        return;
    }
    std::memcpy(record.axes.data(), move, sizeof(record.axes));
    std::memcpy(&record.buttons, static_cast<const std::byte*>(move) + 0x10, sizeof(record.buttons));
}

void submit_move(trace::MoveCodecRecord& record, const trace::Stamp& start) noexcept {
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        record.duration = trace::read_stamp().timestamp - start.timestamp;
        diagnostics->recorder().submit(trace::RecordKind::MoveEncoding, trace::payload_bytes(record), record.packet);
    }
}

void begin_sample(PoseSampler& sampler) noexcept {
    sampler.selected =
        sampler.previous_count == 0 ? 0 : static_cast<std::uint32_t>(sampler.sequence % sampler.previous_count);
    ++sampler.sequence;
    sampler.observed = 0;
    sampler.captured = false;
}

[[nodiscard]] bool select_pose(PoseSampler& sampler) noexcept {
    return sampler.observed++ == sampler.selected;
}

// Records the full local move presented to the native bit writer.
void __cdecl observe_write_move(void* packet, const void* previous, const void* current) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gWriteMove.get(); original != nullptr) {
        original(packet, previous, current);
    }
    const auto identity = identify_move(current);
    trace::MoveCodecRecord record{
        .packet = pointer_id(packet),
        .previous = pointer_id(previous),
        .move = pointer_id(current),
        .kind = 1,
        .player = identity.player,
        .turn_slot = identity.turn_slot,
        .turn_reference = read_global<std::int32_t>(gLayout->client_turn_rva, -1),
    };
    copy_move(record, current);
    submit_move(record, start);
}

// Records the reduced move presented to the mini-move quantizer.
void __cdecl observe_write_mini_move(void* packet, const void* move) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gWriteMiniMove.get(); original != nullptr) {
        original(packet, move);
    }
    const auto identity = identify_move(move);
    trace::MoveCodecRecord record{
        .packet = pointer_id(packet),
        .move = pointer_id(move),
        .kind = 2,
        .player = identity.player,
        .turn_slot = identity.turn_slot,
        .turn_reference = read_global<std::int32_t>(gLayout->client_turn_rva, -1),
    };
    copy_move(record, move);
    submit_move(record, start);
}

// Records the observer-input relay as one decoded batch instead of one record per remote player.
void __cdecl observe_read_player_moves(void* packet) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gReadPlayerMoves.get(); original != nullptr) {
        original(packet);
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        const trace::FunctionRecord record{
            .subject = pointer_id(packet),
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        diagnostics->recorder().submit(trace::RecordKind::RemoteMoveBatch, trace::payload_bytes(record), record.subject,
                                       trace::RecordFlags::End);
    }
}

// Marks one ordinary event decode so action evidence can be correlated with its update.
void __cdecl observe_read_net_event(void* packet) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gReadNetEvent.get(); original != nullptr) {
        original(packet);
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        const trace::FunctionRecord record{
            .subject = pointer_id(packet),
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        diagnostics->recorder().submit(trace::RecordKind::EventDecoding, trace::payload_bytes(record), record.subject);
    }
}

// Records completion of one reliable-event parse and whether it advanced the client's batch frontier.
void __cdecl observe_reliable_events(void* packet) noexcept {
    const auto start = trace::read_stamp();
    const auto frontier_before = read_global<std::uint32_t>(gLayout->reliable_batch_rva);
    if (const auto original = gReceiveReliableEvents.get(); original != nullptr) {
        original(packet);
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        const trace::ReliableEventBatchRecord record{
            .packet = pointer_id(packet),
            .frontier_before = frontier_before,
            .frontier_after = read_global<std::uint32_t>(gLayout->reliable_batch_rva),
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        diagnostics->recorder().submit(trace::RecordKind::ReliableEventBatch, trace::payload_bytes(record),
                                       record.packet, trace::RecordFlags::End);
    }
}

// Distinguishes a complete native fragment group from a normal empty receive poll.
bool __fastcall observe_receive_group(void* group, int packet_type) noexcept {
    const auto start = trace::read_stamp();
    const auto original = gReceiveGroup.get();
    const auto complete = original != nullptr && original(group, packet_type);
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        const trace::NativeReceiveGroupRecord record{
            .group = pointer_id(group),
            .packet_type = packet_type,
            .complete = complete,
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        diagnostics->recorder().submit(trace::RecordKind::NativeReceiveGroup, trace::payload_bytes(record),
                                       record.group,
                                       complete ? trace::RecordFlags::Complete : trace::RecordFlags::Empty);
    }
    return complete;
}

// Captures the authoritative Soldier transform installed by an update decode.
void __fastcall observe_soldier_read_object(void* soldier, void*, void* packet, void* state) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gSoldierReadObject.get(); original != nullptr) {
        original(soldier, nullptr, packet, state);
    }
    if (soldier != nullptr && select_pose(gAuthoritySampler)) {
        gAuthoritySampler.record = {
            .object = pointer_id(soldier),
            .related = pointer_id(packet),
            .stage = 1,
            .player = read_native_field<std::int32_t>(soldier, 0x314),
            .mode = 0,
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        std::memcpy(gAuthoritySampler.record.matrix.data(), static_cast<const std::byte*>(soldier) + 0xF0,
                    sizeof(gAuthoritySampler.record.matrix));
        gAuthoritySampler.captured = true;
    }
}

// Captures the final matrix published for remote Soldier presentation.
void __fastcall observe_render_matrix(void* smooth, void*, const float* matrix) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gSetRenderMatrix.get(); original != nullptr) {
        original(smooth, nullptr, matrix);
    }
    if (smooth == nullptr || matrix == nullptr) {
        return;
    }
    const auto mode = read_native_field<std::uint32_t>(smooth, 0x198);
    if (mode == 0 && select_pose(gPresentationSampler)) {
        gPresentationSampler.record = {
            .object = pointer_id(smooth),
            .related = pointer_id(matrix),
            .stage = 2,
            .player = read_native_field<std::int32_t>(smooth, 0x19C),
            .mode = mode,
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        std::memcpy(gPresentationSampler.record.matrix.data(), matrix, sizeof(gPresentationSampler.record.matrix));
        gPresentationSampler.captured = true;
        if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
            diagnostics->recorder().submit(trace::RecordKind::ClientSoldierPose,
                                           trace::payload_bytes(gPresentationSampler.record),
                                           gPresentationSampler.record.object);
        }
    }
}

} // namespace

void begin_authoritative_update() noexcept {
    begin_sample(gAuthoritySampler);
}

void finish_authoritative_update() noexcept {
    gAuthoritySampler.previous_count = gAuthoritySampler.observed;
    if (!gAuthoritySampler.captured) {
        return;
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        diagnostics->recorder().submit(trace::RecordKind::ClientSoldierPose,
                                       trace::payload_bytes(gAuthoritySampler.record), gAuthoritySampler.record.object);
    }
}

void begin_presentation_frame() noexcept {
    gPresentationSampler.previous_count = gPresentationSampler.observed;
    begin_sample(gPresentationSampler);
}

void build_client_codec_plan(PatchPlan& plan, const TargetContext& target, CaptureMode mode) {
    const auto& layout = client::layout_for(target.layout);
    gLayout = &layout;
    gImage = target.image;
    gWriteMove = plan.inline_hook_with_original("Observe full move encoding", layout.write_move.rva,
                                                layout.write_move.pattern(), &observe_write_move);
    gWriteMiniMove = plan.inline_hook_with_original("Observe mini-move encoding", layout.write_mini_move.rva,
                                                    layout.write_mini_move.pattern(), &observe_write_mini_move);
    gReadPlayerMoves = plan.inline_hook_with_original("Observe remote player moves", layout.read_player_moves.rva,
                                                      layout.read_player_moves.pattern(), &observe_read_player_moves);
    gReadNetEvent = plan.inline_hook_with_original("Observe ordinary event decoding", layout.read_net_event.rva,
                                                   layout.read_net_event.pattern(), &observe_read_net_event);
    gReceiveReliableEvents =
        plan.inline_hook_with_original("Observe reliable event batches", layout.receive_reliable_events.rva,
                                       layout.receive_reliable_events.pattern(), &observe_reliable_events);
    gSoldierReadObject =
        plan.inline_hook_with_original("Observe authoritative Soldier state", layout.soldier_read_object.rva,
                                       layout.soldier_read_object.pattern(), &observe_soldier_read_object);
    gSetRenderMatrix = plan.inline_hook_with_original("Observe visible Soldier state", layout.set_render_matrix.rva,
                                                      layout.set_render_matrix.pattern(), &observe_render_matrix);
    if (mode == CaptureMode::Full) {
        gReceiveGroup = plan.inline_hook_with_original("Observe native fragment groups", layout.receive_group.rva,
                                                       layout.receive_group.pattern(), &observe_receive_group);
    }
}

} // namespace fusioncutter::patches::network_diagnostics
