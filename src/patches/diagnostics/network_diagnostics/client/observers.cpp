#include "../observers.hpp"

#include "codec.hpp"
#include "layout.hpp"
#include "../network_diagnostics.hpp"
#include "../observations.hpp"

#include <algorithm>
#include <cstdint>

namespace fusioncutter::patches::network_diagnostics {
namespace {

using VoidFunction = void(__cdecl*)() noexcept;
using SubmitMoveFunction = void(__cdecl*)(int) noexcept;
using GroupTurnFunction = std::uint32_t(__cdecl*)(void*) noexcept;
using ReadUpdateFunction = void(__cdecl*)(void*) noexcept;
using PredictFunction = bool(__cdecl*)() noexcept;

struct ReceiveDraft {
    NetworkDiagnostics* owner{};
    trace::ClientReceivePassRecord record{};
    trace::Stamp start{};
    bool active{};
};

const client::ClientLayout* gLayout{};
ImageContext gImage{};
OriginalFunction<VoidFunction> gReceiveClient;
OriginalFunction<SubmitMoveFunction> gSubmitMove;
OriginalFunction<GroupTurnFunction> gGetUpdateTurn;
OriginalFunction<ReadUpdateFunction> gReadUpdate;
OriginalFunction<PredictFunction> gPredict;
OriginalFunction<VoidFunction> gAdjustClock;
thread_local ReceiveDraft gReceive;
thread_local std::uint64_t gLastFrame{};

template <typename Value> [[nodiscard]] Value read_global(std::uint32_t rva, Value fallback = {}) noexcept {
    if (rva == 0) {
        return fallback;
    }
    const auto* value = gImage.read_at_rva<Value>(rva);
    return value == nullptr ? fallback : *value;
}

// Records one rendered game-frame boundary without detouring the function's SIMD calling convention.
void observe_game_frame(MidHookContext& context) noexcept {
    auto* diagnostics = active_diagnostics();
    if (diagnostics == nullptr) {
        return;
    }
    const auto stamp = trace::read_stamp();
    if (diagnostics->capture_mode() != CaptureMode::Standard) {
        begin_presentation_frame();
    }
    const trace::ClientFrameRecord record{
        .xmm0_delta = context.xmm0.f32[0],
        .xmm1_delta = context.xmm1.f32[0],
        .gap = gLastFrame == 0 ? 0 : stamp.timestamp - gLastFrame,
    };
    gLastFrame = stamp.timestamp;
    diagnostics->recorder().submit(trace::RecordKind::ClientFrame, trace::payload_bytes(record));
}

// Captures local move production and the client-turn frontier it advances.
void __cdecl observe_submit_move(int requested) noexcept {
    const auto start = trace::read_stamp();
    const auto before = read_global<std::int32_t>(gLayout->client_turn_rva, -1);
    if (const auto original = gSubmitMove.get(); original != nullptr) {
        original(requested);
    }
    auto* diagnostics = active_diagnostics();
    if (diagnostics == nullptr) {
        return;
    }
    const trace::MoveSubmissionRecord record{
        .requested = requested,
        .turn_before = before,
        .turn_after = read_global<std::int32_t>(gLayout->client_turn_rva, -1),
        .accumulator = read_global<float>(gLayout->accumulator_rva),
        .duration = trace::read_stamp().timestamp - start.timestamp,
    };
    diagnostics->recorder().submit(trace::RecordKind::MoveSubmission, trace::payload_bytes(record));
}

// Owns one client update drain and folds candidate and decode observations into it.
void __cdecl observe_receive_client() noexcept {
    auto* diagnostics = active_diagnostics();
    gReceive = {
        .owner = diagnostics,
        .record = {.turn_before = read_global<std::int32_t>(gLayout->client_host_turn_rva, -1)},
        .start = trace::read_stamp(),
        .active = diagnostics != nullptr,
    };
    if (const auto original = gReceiveClient.get(); original != nullptr) {
        original();
    }
    if (!gReceive.active || gReceive.owner == nullptr) {
        return;
    }
    gReceive.record.turn_after = read_global<std::int32_t>(gLayout->client_host_turn_rva, -1);
    gReceive.record.duration = trace::read_stamp().timestamp - gReceive.start.timestamp;
    gReceive.owner->recorder().submit(trace::RecordKind::ClientReceivePass, trace::payload_bytes(gReceive.record), 0,
                                      trace::RecordFlags::End);
    gReceive.active = false;
}

// Adds each complete update candidate to the active receive transaction.
std::uint32_t __cdecl observe_get_update_turn(void* group) noexcept {
    const auto original = gGetUpdateTurn.get();
    const auto turn = original == nullptr ? 0 : original(group);
    if (gReceive.active) {
        ++gReceive.record.candidates;
        gReceive.record.newest_candidate = (std::max)(gReceive.record.newest_candidate, turn);
    }
    return turn;
}

// Classifies the native stale barrier and accepted authority frontier.
void __cdecl observe_read_update(void* group) noexcept {
    const auto before = read_global<std::int32_t>(gLayout->client_host_turn_rva, -1);
    begin_authoritative_update();
    if (const auto original = gReadUpdate.get(); original != nullptr) {
        original(group);
    }
    finish_authoritative_update();
    const auto after = read_global<std::int32_t>(gLayout->client_host_turn_rva, -1);
    if (gReceive.active) {
        if (after > before) {
            ++gReceive.record.accepted;
        } else {
            ++gReceive.record.stale;
        }
        return;
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        const trace::UpdateReadRecord record{
            .turn_before = before,
            .turn_after = after,
            .group = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(group)),
        };
        diagnostics->recorder().submit(trace::RecordKind::UpdateRead, trace::payload_bytes(record), record.group,
                                       after > before ? trace::RecordFlags::Accepted : trace::RecordFlags::Rejected);
    }
}

// Records the authority-replay and local-prediction frontier as one transaction.
bool __cdecl observe_predict() noexcept {
    const auto start = trace::read_stamp();
    const auto before = read_global<std::int32_t>(gLayout->predict_turn_rva, -1);
    const auto original = gPredict.get();
    const auto result = original != nullptr && original();
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        const trace::PredictionRecord record{
            .update_turn = read_global<std::int32_t>(gLayout->update_turn_rva, -1),
            .predict_before = before,
            .predict_after = read_global<std::int32_t>(gLayout->predict_turn_rva, -1),
            .local_turn = read_global<std::int32_t>(gLayout->predict_local_turn_rva, -1),
            .result = result,
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        diagnostics->recorder().submit(trace::RecordKind::Prediction, trace::payload_bytes(record), 0,
                                       result ? trace::RecordFlags::Accepted : trace::RecordFlags::Rejected);
    }
    return result;
}

// Records the native clock controller's effective lag and time-scale correction.
void __cdecl observe_adjust_clock() noexcept {
    trace::ClockAdjustmentRecord record{
        .adjust_before = read_global<std::int32_t>(gLayout->adjust_time_rva),
        .stable_before = read_global<std::int32_t>(gLayout->stable_count_rva),
        .lag_before = read_global<std::int32_t>(gLayout->client_lag_rva),
        .scale_before = read_global<float>(gLayout->time_scale_rva, 1.0F),
    };
    if (const auto original = gAdjustClock.get(); original != nullptr) {
        original();
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        record.adjust_after = read_global<std::int32_t>(gLayout->adjust_time_rva);
        record.stable_after = read_global<std::int32_t>(gLayout->stable_count_rva);
        record.lag_after = read_global<std::int32_t>(gLayout->client_lag_rva);
        record.scale_after = read_global<float>(gLayout->time_scale_rva, 1.0F);
        diagnostics->recorder().submit(trace::RecordKind::ClockAdjustment, trace::payload_bytes(record));
    }
}

} // namespace

void observe_update_recovery(std::uint32_t updates, std::uint32_t oldest_turn, std::uint32_t newest_turn) noexcept {
    if (gReceive.active) {
        gReceive.record.recovered += updates;
        gReceive.record.oldest_recovered = oldest_turn;
        gReceive.record.newest_recovered = newest_turn;
    } else if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        diagnostics->recorder().omit();
    }
}

void build_client_plan(PatchPlan& plan, const TargetContext& target) {
    gLayout = &client::layout_for(target.layout);
    gImage = target.image;
    plan.mid_hook("Observe client game frames", gLayout->game_update.rva, gLayout->game_update.pattern(),
                  &observe_game_frame);
    gSubmitMove = plan.inline_hook_with_original("Observe client move submission", gLayout->submit_move.rva,
                                                 gLayout->submit_move.pattern(), &observe_submit_move);
    gReceiveClient = plan.inline_hook_with_original("Observe client update drains", gLayout->receive_client.rva,
                                                    gLayout->receive_client.pattern(), &observe_receive_client);
    gGetUpdateTurn = plan.inline_hook_with_original("Observe complete update candidates", gLayout->get_update_turn.rva,
                                                    gLayout->get_update_turn.pattern(), &observe_get_update_turn);
    gReadUpdate = plan.inline_hook_with_original("Observe accepted authority updates", gLayout->read_update.rva,
                                                 gLayout->read_update.pattern(), &observe_read_update);
    gPredict = plan.inline_hook_with_original("Observe client prediction", gLayout->predict.rva,
                                              gLayout->predict.pattern(), &observe_predict);
    gAdjustClock = plan.inline_hook_with_original("Observe client clock adjustment", gLayout->adjust_clock.rva,
                                                  gLayout->adjust_clock.pattern(), &observe_adjust_clock);
}

} // namespace fusioncutter::patches::network_diagnostics
