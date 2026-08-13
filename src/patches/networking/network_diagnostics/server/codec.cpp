#include "../observers.hpp"

#include "codec.hpp"
#include "layout.hpp"
#include "../network_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fusioncutter::patches::network_diagnostics {
namespace {

using VoidFunction = void(__cdecl*)() noexcept;
using PacketFunction = void(__cdecl*)(void*) noexcept;
using ReadMove = bool(__cdecl*)(void*, const void*, void*) noexcept;
using ReadMiniMove = void(__cdecl*)(void*, void*) noexcept;
using FillTurns = void(__cdecl*)(int, std::uint32_t) noexcept;
using ReceiveGroup = bool(__fastcall*)(void*, int) noexcept;

const server::ServerLayout* gLayout{};
ImageContext gImage{};
OriginalFunction<ReadMove> gReadMove;
OriginalFunction<ReadMiniMove> gReadMiniMove;
OriginalFunction<FillTurns> gFillTurns;
OriginalFunction<VoidFunction> gReceivePrimary;
OriginalFunction<ReceiveGroup> gReceiveGroup;
OriginalFunction<PacketFunction> gWriteObjects;
OriginalFunction<PacketFunction> gWritePlayerMoves;
OriginalFunction<PacketFunction> gSendReliableEvents;
OriginalFunction<PacketFunction> gSendEvents;
thread_local trace::ServerInputRecord gReceive;
thread_local trace::DestinationUpdateRecord gOutput;
thread_local bool gReceiving{};
thread_local bool gWriting{};

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

// Identifies a decoded move by its fixed player and slot in the server history ring.
[[nodiscard]] MoveIdentity identify_move(const void* move) noexcept {
    constexpr std::size_t kTurnCount = 32;
    constexpr std::size_t kTurnStride = 0x600;
    constexpr std::size_t kPlayerCount = 64;
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
    if (player_offset % kPlayerStride != 0 || player_offset / kPlayerStride >= kPlayerCount) {
        return {};
    }
    return {
        .player = static_cast<std::int32_t>(player_offset / kPlayerStride),
        .turn_slot = static_cast<std::int32_t>(offset / kTurnStride),
    };
}

[[nodiscard]] MoveIdentity decoded_move_identity(const void* move) noexcept {
    auto identity = identify_move(move);
    if (identity.player < 0) {
        // Late moves decode into a stack temporary instead of the player's history-ring slot.
        const auto participant = read_global<std::int32_t>(gLayout->current_destination_rva, -1);
        if (participant >= 0 && participant < 64) {
            identity.player = participant;
        }
    }
    return identity;
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
        diagnostics->recorder().submit(trace::RecordKind::MoveDecoding, trace::payload_bytes(record), record.packet);
    }
}

// Records the full move reconstructed from one client packet.
bool __cdecl observe_read_move(void* packet, const void* previous, void* output) noexcept {
    const auto start = trace::read_stamp();
    const auto original = gReadMove.get();
    const auto decoded = original != nullptr && original(packet, previous, output);
    if (!decoded) {
        return false;
    }
    if (gReceiving) {
        ++gReceive.moves;
    }
    const auto identity = decoded_move_identity(output);
    trace::MoveCodecRecord record{
        .packet = pointer_id(packet),
        .previous = pointer_id(previous),
        .move = pointer_id(output),
        .kind = 3,
        .player = identity.player,
        .turn_slot = identity.turn_slot,
        .turn_reference = read_global<std::int32_t>(gLayout->host_turn_rva, -1),
    };
    copy_move(record, output);
    submit_move(record, start);
    return true;
}

// Records the move reconstructed through the mini-move codec.
void __cdecl observe_read_mini_move(void* packet, void* output) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gReadMiniMove.get(); original != nullptr) {
        original(packet, output);
    }
    if (gReceiving) {
        ++gReceive.mini_moves;
    }
    const auto identity = decoded_move_identity(output);
    trace::MoveCodecRecord record{
        .packet = pointer_id(packet),
        .move = pointer_id(output),
        .kind = 4,
        .player = identity.player,
        .turn_slot = identity.turn_slot,
        .turn_reference = read_global<std::int32_t>(gLayout->host_turn_rva, -1),
    };
    copy_move(record, output);
    submit_move(record, start);
}

// Counts only the missing moves the native routine actually substitutes for this player.
void __cdecl observe_fill_turns(int player, std::uint32_t end_turn) noexcept {
    constexpr std::uint32_t kPlayerCount = 64;
    constexpr std::uint32_t kPlayerStride = 0x208;
    const auto valid_player = player >= 0 && player < static_cast<int>(kPlayerCount);
    const auto counter_rva =
        valid_player ? gLayout->filled_turn_count_rva + static_cast<std::uint32_t>(player) * kPlayerStride : 0;
    const auto before = valid_player ? read_global<std::uint32_t>(counter_rva) : 0;
    if (const auto original = gFillTurns.get(); original != nullptr) {
        original(player, end_turn);
    }
    if (!gReceiving || !valid_player) {
        return;
    }
    const auto after = read_global<std::uint32_t>(counter_rva, before);
    const auto filled = after >= before ? after - before : 0;
    if (filled != 0) {
        ++gReceive.filled_players;
        gReceive.filled_turns += filled;
    }
}

// Owns the server's normal client-packet parser transaction.
void __cdecl observe_receive_primary() noexcept {
    const auto start = trace::read_stamp();
    gReceive = {};
    gReceiving = true;
    if (const auto original = gReceivePrimary.get(); original != nullptr) {
        original();
    }
    gReceiving = false;
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        gReceive.duration = trace::read_stamp().timestamp - start.timestamp;
        diagnostics->recorder().submit(trace::RecordKind::ServerInputPass, trace::payload_bytes(gReceive), 0,
                                       trace::RecordFlags::End);
    }
}

// Captures only the client acknowledgement reconstruction consumed by RTT and send retirement.
void observe_ack_reconstruction(MidHookContext& context) noexcept {
    if (!gReceiving || context.ebp == 0) {
        return;
    }
    const auto* frame = reinterpret_cast<const std::byte*>(context.ebp);
    auto turn = read_native_field<std::int32_t>(frame - 0x30);
    if (turn == -1) {
        turn = 0;
    }
    const auto player = read_native_field<std::int32_t>(frame - 0x10);
    if (gReceive.acknowledgements++ == 0) {
        gReceive.oldest_acknowledged_turn = turn;
        gReceive.newest_acknowledged_turn = turn;
    } else {
        gReceive.oldest_acknowledged_turn = (std::min)(gReceive.oldest_acknowledged_turn, turn);
        gReceive.newest_acknowledged_turn = (std::max)(gReceive.newest_acknowledged_turn, turn);
    }
    if (player >= 0 && player < 64) {
        gReceive.acknowledged_players |= std::uint64_t{1} << static_cast<std::uint32_t>(player);
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

void __cdecl observe_write_objects(void* packet) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gWriteObjects.get(); original != nullptr) {
        original(packet);
    }
    if (gWriting) {
        gOutput.stages |= 1U;
        gOutput.objects_duration = trace::read_stamp().timestamp - start.timestamp;
    }
}

void __cdecl observe_send_reliable_events(void* packet) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gSendReliableEvents.get(); original != nullptr) {
        original(packet);
    }
    if (gWriting) {
        gOutput.stages |= 2U;
        gOutput.reliable_duration = trace::read_stamp().timestamp - start.timestamp;
    }
}

void __cdecl observe_write_player_moves(void* packet) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gWritePlayerMoves.get(); original != nullptr) {
        original(packet);
    }
    if (gWriting) {
        gOutput.stages |= 4U;
        gOutput.player_moves_duration = trace::read_stamp().timestamp - start.timestamp;
    }
}

void __cdecl observe_send_events(void* packet) noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gSendEvents.get(); original != nullptr) {
        original(packet);
    }
    if (gWriting) {
        gOutput.stages |= 8U;
        gOutput.events_duration = trace::read_stamp().timestamp - start.timestamp;
    }
}

} // namespace

void begin_destination_update(std::int32_t destination) noexcept {
    auto* diagnostics = active_diagnostics();
    if (diagnostics == nullptr || diagnostics->capture_mode() != CaptureMode::Full) {
        return;
    }
    const auto valid_destination = destination >= 0 && destination < 64;
    gOutput = {
        .destination = destination,
        .event_cursor_before = valid_destination
                                   ? read_global<std::uint32_t>(gLayout->event_cursor_rva +
                                                                static_cast<std::uint32_t>(destination) * 0x208U)
                                   : 0,
    };
    gWriting = true;
}

void finish_destination_update() noexcept {
    if (!gWriting) {
        return;
    }
    if (gOutput.destination >= 0 && gOutput.destination < 64) {
        gOutput.event_cursor_after = read_global<std::uint32_t>(
            gLayout->event_cursor_rva + static_cast<std::uint32_t>(gOutput.destination) * 0x208U);
    }
    gOutput.event_head = read_global<std::uint32_t>(gLayout->event_head_rva);
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        diagnostics->recorder().submit(trace::RecordKind::DestinationUpdate, trace::payload_bytes(gOutput),
                                       static_cast<std::uint32_t>(gOutput.destination), trace::RecordFlags::End);
    }
    gWriting = false;
}

void build_server_codec_plan(PatchPlan& plan, const TargetContext& target, CaptureMode mode) {
    gLayout = &server::kGogLayout;
    gImage = target.image;
    gReadMove = plan.inline_hook_with_original("Observe full move decoding", gLayout->read_move.rva,
                                               gLayout->read_move.pattern(), &observe_read_move);
    gReadMiniMove = plan.inline_hook_with_original("Observe mini-move decoding", gLayout->read_mini_move.rva,
                                                   gLayout->read_mini_move.pattern(), &observe_read_mini_move);
    if (mode != CaptureMode::Full) {
        return;
    }

    gFillTurns = plan.inline_hook_with_original("Observe missing-turn substitution", gLayout->fill_turns.rva,
                                                gLayout->fill_turns.pattern(), &observe_fill_turns);
    gReceivePrimary = plan.inline_hook_with_original("Observe client packet parsing", gLayout->receive_primary.rva,
                                                     gLayout->receive_primary.pattern(), &observe_receive_primary);
    plan.mid_hook("Observe client acknowledgement reconstruction", gLayout->ack_reconstruction.rva,
                  gLayout->ack_reconstruction.pattern(), &observe_ack_reconstruction);
    gReceiveGroup = plan.inline_hook_with_original("Observe native fragment groups", gLayout->receive_group.rva,
                                                   gLayout->receive_group.pattern(), &observe_receive_group);
    gWriteObjects = plan.inline_hook_with_original("Observe object replication", gLayout->write_objects.rva,
                                                   gLayout->write_objects.pattern(), &observe_write_objects);
    gSendReliableEvents =
        plan.inline_hook_with_original("Observe reliable event replication", gLayout->send_reliable_events.rva,
                                       gLayout->send_reliable_events.pattern(), &observe_send_reliable_events);
    gWritePlayerMoves =
        plan.inline_hook_with_original("Observe player move replication", gLayout->write_player_moves.rva,
                                       gLayout->write_player_moves.pattern(), &observe_write_player_moves);
    gSendEvents = plan.inline_hook_with_original("Observe ordinary event replication", gLayout->send_events.rva,
                                                 gLayout->send_events.pattern(), &observe_send_events);
}

} // namespace fusioncutter::patches::network_diagnostics
