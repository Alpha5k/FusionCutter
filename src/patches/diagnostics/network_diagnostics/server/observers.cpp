#include "../observers.hpp"

#include "combat.hpp"
#include "codec.hpp"
#include "layout.hpp"
#include "../network_diagnostics.hpp"
#include "../observations.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>

namespace fusioncutter::patches::network_diagnostics {
namespace {

using VoidFunction = void(__cdecl*)() noexcept;
using SubmitMoveFunction = void(__cdecl*)(int) noexcept;
using WriteUpdateFunction = void(__cdecl*)(void*) noexcept;

struct ReadinessSnapshot {
    std::int32_t host_turn{-1};
    std::int32_t client_turn{-1};
    std::uint64_t required_mask{};
    std::uint64_t received_mask{};
    std::uint64_t missing_mask{};
    std::uint64_t new_input_mask{};
    std::uint32_t lead_turns{};
    std::uint8_t grace_turns{};
    bool waitlate{};
    bool network_enabled{};
    bool force_eligible{};
};

struct ServerState {
    trace::ServerFrameRecord frame{};
    trace::AuthorityTurnRecord turn{};
    trace::SchedulingRecord scheduling{};
    trace::Stamp frame_start{};
    trace::Stamp save_end{};
    std::uint64_t received_before{};
    bool frame_active{};
    bool turn_active{};
    bool sending{};
    bool object_budget_observed{};
};

const server::ServerLayout* gLayout{};
ImageContext gImage{};
OriginalFunction<SubmitMoveFunction> gSubmitMove;
OriginalFunction<VoidFunction> gReceiveHost;
OriginalFunction<VoidFunction> gRollbackLateMoves;
OriginalFunction<VoidFunction> gSaveLateState;
OriginalFunction<VoidFunction> gFinishTurn;
OriginalFunction<VoidFunction> gSendHost;
OriginalFunction<WriteUpdateFunction> gWriteUpdate;
thread_local ServerState gState;
thread_local std::uint64_t gLastPolicy{};
thread_local bool gHasPolicy{};

template <typename Value> [[nodiscard]] Value read_global(std::uint32_t rva, Value fallback = {}) noexcept {
    const auto* value = gImage.read_at_rva<Value>(rva);
    return value == nullptr ? fallback : *value;
}

[[nodiscard]] std::uint64_t received_mask(std::int32_t turn) noexcept {
    if (turn < 0) {
        return 0;
    }
    return read_global<std::uint64_t>(gLayout->received_masks_rva + (static_cast<std::uint32_t>(turn) & 31U) * 8U);
}

[[nodiscard]] ReadinessSnapshot capture_readiness(std::uint64_t before_received = 0) noexcept {
    ReadinessSnapshot snapshot{
        .host_turn = read_global<std::int32_t>(gLayout->host_turn_rva, -1),
        .client_turn = read_global<std::int32_t>(gLayout->client_turn_rva, -1),
        .required_mask = read_global<std::uint64_t>(gLayout->required_mask_rva),
        .lead_turns = read_global<std::uint32_t>(gLayout->lead_turns_rva),
        .grace_turns = read_global<std::uint8_t>(gLayout->waitlate_grace_rva),
        .waitlate = read_global<std::uint8_t>(gLayout->waitlate_rva) != 0,
        .network_enabled = read_global<std::uint8_t>(gLayout->network_enabled_rva) != 0,
    };
    snapshot.received_mask = received_mask(snapshot.host_turn) & snapshot.required_mask;
    snapshot.missing_mask = snapshot.required_mask & ~snapshot.received_mask;
    snapshot.new_input_mask = snapshot.received_mask & ~before_received;
    const auto grace = snapshot.waitlate ? snapshot.grace_turns : 0U;
    snapshot.force_eligible = snapshot.missing_mask != 0 && snapshot.client_turn > static_cast<std::int32_t>(grace) &&
                              snapshot.host_turn < snapshot.client_turn - static_cast<std::int32_t>(grace);
    return snapshot;
}

[[nodiscard]] trace::ReadinessFailure classify_readiness(const ReadinessSnapshot& snapshot) noexcept {
    const auto leading_turn = static_cast<std::int64_t>(snapshot.client_turn) - snapshot.lead_turns;
    if (snapshot.host_turn > leading_turn) {
        return trace::ReadinessFailure::NoLeadingTurn;
    }
    if (snapshot.missing_mask != 0) {
        return snapshot.force_eligible ? trace::ReadinessFailure::ForceFillFailed
                                       : trace::ReadinessFailure::MissingInput;
    }
    return trace::ReadinessFailure::ReadinessGate;
}

[[nodiscard]] std::uint32_t policy_bits(const ReadinessSnapshot& snapshot) noexcept {
    return (snapshot.waitlate ? 1U : 0U) | (static_cast<std::uint32_t>(snapshot.grace_turns) << 8U) |
           (snapshot.network_enabled ? 1U << 16U : 0U);
}

void submit_policy_if_changed(NetworkDiagnostics& diagnostics, const ReadinessSnapshot& readiness) noexcept {
    const auto fixed_delta = read_global<float>(gLayout->fixed_delta_rva);
    const auto key =
        (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(fixed_delta)) << 32U) | policy_bits(readiness);
    if (gHasPolicy && gLastPolicy == key) {
        return;
    }
    gHasPolicy = true;
    gLastPolicy = key;
    const trace::NetworkPolicyRecord record{
        .waitlate = readiness.waitlate,
        .grace_turns = readiness.grace_turns,
        .network_enabled = readiness.network_enabled,
        .fixed_delta = fixed_delta,
    };
    diagnostics.recorder().submit(trace::RecordKind::NetworkPolicy, trace::payload_bytes(record));
}

void add_requested_turns(int requested) noexcept {
    if (requested <= 0) {
        return;
    }
    const auto total = static_cast<std::uint32_t>(gState.frame.requested_turns) + static_cast<std::uint32_t>(requested);
    gState.frame.requested_turns = static_cast<std::uint16_t>(
        (std::min)(total, static_cast<std::uint32_t>((std::numeric_limits<std::uint16_t>::max)())));
}

void begin_frame(const trace::Stamp& stamp) noexcept {
    const auto previous = gState.frame_start.timestamp;
    gState = {};
    gState.frame = {
        .host_turn_before = read_global<std::int32_t>(gLayout->host_turn_rva, -1),
        .frame_gap = previous == 0 ? 0 : stamp.timestamp - previous,
    };
    gState.frame_start = stamp;
    gState.frame_active = true;
}

// Establishes the outer host-frame transaction while preserving the game's SIMD entry ABI.
void observe_game_frame(MidHookContext&) noexcept {
    if (active_diagnostics() != nullptr) {
        begin_frame(trace::read_stamp());
    }
}

// Records the due-turn request that seeds this host frame.
void __cdecl observe_submit_move(int requested) noexcept {
    const auto start = trace::read_stamp();
    const auto turn_before = read_global<std::int32_t>(gLayout->client_turn_rva, -1);
    if (!gState.frame_active && active_diagnostics() != nullptr) {
        begin_frame(start);
    }
    if (gState.frame_active) {
        add_requested_turns(requested);
    }
    if (const auto original = gSubmitMove.get(); original != nullptr) {
        original(requested);
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        const trace::MoveSubmissionRecord record{
            .requested = requested,
            .turn_before = turn_before,
            .turn_after = read_global<std::int32_t>(gLayout->client_turn_rva, -1),
            .accumulator = read_global<float>(gLayout->accumulator_rva),
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        diagnostics->recorder().submit(trace::RecordKind::MoveSubmission, trace::payload_bytes(record));
    }
}

// Updates readiness masks and receive cost for the current authority attempt.
void __cdecl observe_receive_host() noexcept {
    const auto start = trace::read_stamp();
    const auto host_turn = read_global<std::int32_t>(gLayout->host_turn_rva, -1);
    const auto required = read_global<std::uint64_t>(gLayout->required_mask_rva);
    gState.received_before = received_mask(host_turn) & required;
    if (const auto original = gReceiveHost.get(); original != nullptr) {
        original();
    }
    if (!gState.frame_active) {
        return;
    }
    const auto duration = trace::read_stamp().timestamp - start.timestamp;
    ++gState.frame.receive_calls;
    gState.turn.receive_duration += duration;
    const auto received = received_mask(host_turn) & required;
    gState.turn.new_input_mask |= received & ~gState.received_before;
}

// Measures the native late-move rollback pass without changing its policy or execution.
void __cdecl observe_rollback_late_moves() noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gRollbackLateMoves.get(); original != nullptr) {
        original();
    }
    if (gState.frame_active) {
        gState.turn.rollback_duration += trace::read_stamp().timestamp - start.timestamp;
    }
}

// Uses the success-only SaveLateState boundary to begin one accepted authority turn.
void __cdecl observe_save_late_state() noexcept {
    const auto start = trace::read_stamp();
    const auto candidate = read_global<std::int32_t>(gLayout->host_turn_rva, -1);
    const auto required = read_global<std::uint64_t>(gLayout->required_mask_rva);
    const auto received = received_mask(candidate) & required;
    gState.turn.candidate_turn = candidate;
    gState.turn.client_turn = read_global<std::int32_t>(gLayout->client_turn_rva, -1);
    gState.turn.requested_turns = static_cast<std::int32_t>(gState.frame.requested_turns);
    gState.turn.catch_up_ordinal = static_cast<std::uint32_t>(gState.frame.turns_advanced) + 1U;
    gState.turn.required_mask = required;
    gState.turn.received_mask = received;
    gState.turn.missing_mask = required & ~received;
    if (const auto original = gSaveLateState.get(); original != nullptr) {
        original();
    }
    gState.save_end = trace::read_stamp();
    gState.turn.save_duration = gState.save_end.timestamp - start.timestamp;
    gState.turn_active = true;
    ++gState.frame.turns_advanced;
}

// Completes the accepted authority-turn record after world simulation finishes.
void __cdecl observe_finish_turn() noexcept {
    const auto start = trace::read_stamp();
    if (const auto original = gFinishTurn.get(); original != nullptr) {
        original();
    }
    if (!gState.turn_active) {
        return;
    }
    const auto end = trace::read_stamp();
    gState.turn.simulation_duration = start.timestamp - gState.save_end.timestamp;
    gState.turn.finish_duration = end.timestamp - start.timestamp;
    gState.turn.host_turn_after = read_global<std::int32_t>(gLayout->host_turn_rva, -1);
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        diagnostics->recorder().submit(trace::RecordKind::AuthorityTurn, trace::payload_bytes(gState.turn),
                                       static_cast<std::uint32_t>(gState.turn.candidate_turn),
                                       trace::RecordFlags::Accepted);
        if (diagnostics->capture_mode() != CaptureMode::Standard) {
            write_authoritative_poses(*diagnostics, gState.turn.candidate_turn);
        }
    }
    gState.turn = {};
    gState.turn_active = false;
}

// Folds one destination's update cost into the owning SendHost transaction.
void __cdecl observe_write_update(void* packet) noexcept {
    const auto start = trace::read_stamp();
    const auto destination = read_global<std::int32_t>(gLayout->current_destination_rva, -1);
    begin_destination_update(destination);
    if (const auto original = gWriteUpdate.get(); original != nullptr) {
        original(packet);
    }
    finish_destination_update();
    if (!gState.sending) {
        return;
    }
    const auto duration = trace::read_stamp().timestamp - start.timestamp;
    ++gState.frame.write_updates;
    gState.frame.write_update_duration += duration;
    if (destination >= 0 && destination < 64) {
        gState.frame.destination_mask |= std::uint64_t{1} << static_cast<std::uint32_t>(destination);
    }
}

// Completes the outer frame with one packed destination-send summary.
void __cdecl observe_send_host() noexcept {
    const auto start = trace::read_stamp();
    gState.sending = true;
    if (const auto original = gSendHost.get(); original != nullptr) {
        original();
    }
    gState.sending = false;
    if (!gState.frame_active) {
        return;
    }

    const auto end = trace::read_stamp();
    const auto host_turn = read_global<std::int32_t>(gLayout->host_turn_rva, -1);
    const auto readiness = capture_readiness();
    const auto maximum_step_turns = read_global<std::uint8_t>(gLayout->maximum_step_turns_rva, 1);
    gState.frame.host_turn_after = host_turn;
    gState.frame.client_turn = read_global<std::int32_t>(gLayout->client_turn_rva, -1);
    gState.frame.send_duration = end.timestamp - start.timestamp;
    gState.frame.required_mask = readiness.required_mask;
    gState.frame.missing_mask = readiness.missing_mask;
    gState.frame.policy = policy_bits(readiness);
    if (gState.frame.turns_advanced < maximum_step_turns) {
        const auto failure = classify_readiness(readiness);
        if (failure != trace::ReadinessFailure::NoLeadingTurn) {
            gState.frame.terminal_failure = failure;
        }
    }
    gState.frame.accumulator = read_global<float>(gLayout->accumulator_rva);
    gState.frame.fixed_delta = read_global<float>(gLayout->fixed_delta_rva);
    gState.frame.simulation_delta = read_global<float>(gLayout->simulation_delta_rva);
    gState.frame.time_scale = read_global<float>(gLayout->time_scale_rva, 1.0F);
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        submit_policy_if_changed(*diagnostics, readiness);
        std::uint16_t flags = trace::RecordFlags::End;
        if (gState.frame.turns_advanced == 0 && gState.frame.missing_mask != 0) {
            flags |= trace::RecordFlags::Anomaly;
        }
        diagnostics->recorder().submit(trace::RecordKind::ServerFrame, trace::payload_bytes(gState.frame),
                                       static_cast<std::uint32_t>(host_turn), flags);
        const auto& scheduling = gState.scheduling;
        if (scheduling.ack_slots != 0 || scheduling.create_fences != 0 || scheduling.blocked_destinations != 0 ||
            scheduling.maximum_pending_events != 0) {
            diagnostics->recorder().submit(trace::RecordKind::SendPass, trace::payload_bytes(scheduling),
                                           static_cast<std::uint32_t>(host_turn));
        }
    }
    gState.frame_active = false;
}

} // namespace

void observe_ack_slot(std::int32_t, bool accepted) noexcept {
    if (!gState.frame_active) {
        return;
    }
    ++gState.scheduling.ack_slots;
    gState.scheduling.rejected_slots += accepted ? 0U : 1U;
}

void observe_create_fence(std::uint32_t destination, std::int32_t) noexcept {
    if (gState.frame_active) {
        ++gState.scheduling.create_fences;
        if (destination < 64) {
            gState.scheduling.destination_mask |= std::uint64_t{1} << destination;
        }
    }
}

void observe_destination_gate(std::uint32_t destination, bool blocked, bool timed_out) noexcept {
    if (!gState.frame_active) {
        return;
    }
    gState.scheduling.blocked_destinations += blocked ? 1U : 0U;
    gState.scheduling.timed_out_fences += timed_out ? 1U : 0U;
    if ((blocked || timed_out) && destination < 64) {
        gState.scheduling.destination_mask |= std::uint64_t{1} << destination;
    }
}

void observe_object_budget(std::uint32_t destination, std::uint32_t pending_events,
                           std::int32_t object_scale) noexcept {
    if (!gState.frame_active) {
        return;
    }
    gState.scheduling.maximum_pending_events = (std::max)(gState.scheduling.maximum_pending_events, pending_events);
    auto& minimum_scale = gState.scheduling.minimum_object_scale;
    minimum_scale = gState.object_budget_observed ? (std::min)(minimum_scale, object_scale) : object_scale;
    gState.object_budget_observed = true;
    if (pending_events != 0 && destination < 64) {
        gState.scheduling.destination_mask |= std::uint64_t{1} << destination;
    }
}

void build_server_plan(PatchPlan& plan, const TargetContext& target) {
    gLayout = &server::kGogLayout;
    gImage = target.image;
    plan.mid_hook("Observe server game frames", gLayout->game_update.rva, gLayout->game_update.pattern(),
                  &observe_game_frame);
    gSubmitMove = plan.inline_hook_with_original("Observe server move submission", gLayout->submit_move.rva,
                                                 gLayout->submit_move.pattern(), &observe_submit_move);
    gReceiveHost = plan.inline_hook_with_original("Observe server receive passes", gLayout->receive_host.rva,
                                                  gLayout->receive_host.pattern(), &observe_receive_host);
    gRollbackLateMoves =
        plan.inline_hook_with_original("Observe late-move rollback", gLayout->rollback_late_moves.rva,
                                       gLayout->rollback_late_moves.pattern(), &observe_rollback_late_moves);
    gSaveLateState = plan.inline_hook_with_original("Observe accepted authority turns", gLayout->save_late_state.rva,
                                                    gLayout->save_late_state.pattern(), &observe_save_late_state);
    gFinishTurn = plan.inline_hook_with_original("Observe completed authority turns", gLayout->finish_turn.rva,
                                                 gLayout->finish_turn.pattern(), &observe_finish_turn);
    gSendHost = plan.inline_hook_with_original("Observe server send passes", gLayout->send_host.rva,
                                               gLayout->send_host.pattern(), &observe_send_host);
    gWriteUpdate = plan.inline_hook_with_original("Observe destination updates", gLayout->write_update.rva,
                                                  gLayout->write_update.pattern(), &observe_write_update);
}

} // namespace fusioncutter::patches::network_diagnostics
