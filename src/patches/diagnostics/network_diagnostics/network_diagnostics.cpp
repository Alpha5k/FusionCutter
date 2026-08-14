#include "network_diagnostics.hpp"

#include "client/codec.hpp"
#include "observers.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace fusioncutter::patches::network_diagnostics {
namespace {

struct GroupDraft {
    NetworkDiagnostics* owner{};
    trace::Stamp start{};
    trace::NativeSendGroupRecord record{};
    bool active{};
};

struct ReceiveDraft {
    NetworkDiagnostics* owner{};
    trace::Stamp start{};
    trace::TransportReceiveRecord record{};
    bool active{};
};

thread_local GroupDraft gGroup;
thread_local ReceiveDraft gReceive;

template <typename Value>
[[nodiscard]] std::string_view number_text(Value value, std::array<char, 32>& buffer) noexcept {
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return error == std::errc{} ? std::string_view{buffer.data(), end} : std::string_view{"Unavailable"};
}

[[nodiscard]] std::string_view capture_name(CaptureMode mode) noexcept {
    switch (mode) {
    case CaptureMode::Standard:
        return "Standard";
    case CaptureMode::Combat:
        return "Combat";
    case CaptureMode::Full:
        return "Full";
    }
    std::unreachable();
}

[[nodiscard]] NetworkDiagnostics& diagnostics_from(void* context) noexcept {
    return *static_cast<NetworkDiagnostics*>(context);
}

[[nodiscard]] trace::Carrier trace_carrier(network_pipeline::PacketCarrier carrier) noexcept {
    switch (carrier) {
    case network_pipeline::PacketCarrier::Native:
        return trace::Carrier::Native;
    case network_pipeline::PacketCarrier::Direct:
        return trace::Carrier::Direct;
    case network_pipeline::PacketCarrier::NotSent:
        return trace::Carrier::NotSent;
    }
    std::unreachable();
}

[[nodiscard]] trace::DirectRoute direct_route(std::uint8_t route) noexcept {
    switch (route) {
    case 1:
        return trace::DirectRoute::Unclassified;
    case 2:
    case 3:
        return trace::DirectRoute::Negotiating;
    case 4:
        return trace::DirectRoute::Native;
    case 5:
        return trace::DirectRoute::Direct;
    default:
        return trace::DirectRoute::Unclassified;
    }
}

[[nodiscard]] trace::DirectAssociationPhase direct_phase(network_pipeline::DirectAssociationPhase phase) noexcept {
    switch (phase) {
    case network_pipeline::DirectAssociationPhase::Started:
        return trace::DirectAssociationPhase::Started;
    case network_pipeline::DirectAssociationPhase::Terminal:
        return trace::DirectAssociationPhase::Terminal;
    case network_pipeline::DirectAssociationPhase::Ended:
        return trace::DirectAssociationPhase::Ended;
    }
    std::unreachable();
}

[[nodiscard]] trace::OutputPacingOutcome pacing_outcome(network_pipeline::OutputPacingOutcome outcome) noexcept {
    switch (outcome) {
    case network_pipeline::OutputPacingOutcome::Immediate:
        return trace::OutputPacingOutcome::Immediate;
    case network_pipeline::OutputPacingOutcome::Held:
        return trace::OutputPacingOutcome::Held;
    case network_pipeline::OutputPacingOutcome::CapLimited:
        return trace::OutputPacingOutcome::CapLimited;
    case network_pipeline::OutputPacingOutcome::QueueCollision:
        return trace::OutputPacingOutcome::QueueCollision;
    case network_pipeline::OutputPacingOutcome::LifecycleDiscard:
        return trace::OutputPacingOutcome::LifecycleDiscard;
    case network_pipeline::OutputPacingOutcome::ModeTransition:
        return trace::OutputPacingOutcome::ModeTransition;
    case network_pipeline::OutputPacingOutcome::CapacityExceeded:
        return trace::OutputPacingOutcome::CapacityExceeded;
    }
    std::unreachable();
}

[[nodiscard]] trace::Carrier association_carrier(trace::DirectRoute route) noexcept {
    switch (route) {
    case trace::DirectRoute::Native:
        return trace::Carrier::Native;
    case trace::DirectRoute::Direct:
        return trace::Carrier::Direct;
    case trace::DirectRoute::Unclassified:
    case trace::DirectRoute::Negotiating:
        return trace::Carrier::Unknown;
    }
    std::unreachable();
}

network_pipeline::DiagnosticsCallbacks make_pipeline_callbacks(NetworkDiagnostics& diagnostics) noexcept {
    return {
        .context = &diagnostics,
        .group =
            [](void* context, int destination, bool begin) noexcept {
                diagnostics_from(context).observe_group(destination, begin);
            },
        .send =
            [](void* context, int destination, std::size_t bytes, network_pipeline::PacketCarrier carrier,
               int result) noexcept {
                diagnostics_from(context).observe_send(destination, bytes, carrier, result);
            },
        .receive =
            [](void* context, bool begin) noexcept {
                diagnostics_from(context).observe_receive(begin);
            },
        .intake =
            [](void* context, void* endpoint, bool direct) noexcept {
                diagnostics_from(context).observe_intake(endpoint, direct);
            },
        .disconnect =
            [](void* context, int destination, bool begin) noexcept {
                diagnostics_from(context).observe_disconnect(destination, begin);
            },
        .reset =
            [](void* context, std::uint8_t mode) noexcept {
                diagnostics_from(context).observe_reset(mode);
            },
        .direct_association =
            [](void* context, const network_pipeline::DirectAssociationObservation& observation) noexcept {
                diagnostics_from(context).observe_direct_association(observation);
            },
        .direct_receive =
            [](void* context, const network_pipeline::DirectReceiveObservation& observation) noexcept {
                diagnostics_from(context).observe_direct_receive(observation);
            },
        .output_pacing =
            [](void* context, const network_pipeline::OutputPacingObservation& observation) noexcept {
                diagnostics_from(context).observe_output_pacing(observation);
            },
    };
}

} // namespace

NetworkDiagnostics::NetworkDiagnostics(NetworkDiagnosticsSettings settings, const TargetContext& target) noexcept
    : settings_(settings), target_(target), pipeline_callbacks_(make_pipeline_callbacks(*this))
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 1) != 0
      ,
      soldier_state_observer_(make_soldier_state_observer(*this))
#endif
{
}

NetworkDiagnostics::~NetworkDiagnostics() {
    disable_runtime();
}

void NetworkDiagnostics::build_plan(PatchPlan& plan) {
    if (target_.role == HostRole::Client) {
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 1) != 0
        build_client_plan(plan, target_);
        if (settings_.capture != CaptureMode::Standard) {
            build_client_codec_plan(plan, target_, settings_.capture);
            build_client_combat_plan(plan, target_);
        }
#endif
    } else {
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 2) != 0
        build_server_plan(plan, target_);
        if (settings_.capture != CaptureMode::Standard) {
            build_server_codec_plan(plan, target_, settings_.capture);
            build_server_combat_plan(plan, target_);
        }
#endif
    }
}

std::expected<void, OutcomeReason> NetworkDiagnostics::prepare_runtime() {
    return recorder_.prepare(target_, static_cast<std::uint8_t>(std::to_underlying(settings_.capture)));
}

void NetworkDiagnostics::enable_runtime() noexcept {
    recorder_.start();
    publish_observers(*this);
    network_pipeline::publish_diagnostics(pipeline_callbacks_);
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 1) != 0
    if (target_.role == HostRole::Client && settings_.capture != CaptureMode::Standard) {
        soldier_state_pipeline::publish_observer(soldier_state_observer_);
    }
#endif
}

void NetworkDiagnostics::disable_runtime() noexcept {
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 1) != 0
    if (target_.role == HostRole::Client) {
        soldier_state_pipeline::clear_observer(soldier_state_observer_);
    }
#endif
    network_pipeline::clear_diagnostics(pipeline_callbacks_);
    clear_observers(*this);
    recorder_.stop();
}

void NetworkDiagnostics::write_status(StatusSection& output) const noexcept {
    const auto health = recorder_.health();
    std::array<char, 32> buffer{};
    output.add("Capture", capture_name(settings_.capture));
    output.add("Trace", recorder_.filename());
    output.add("EmittedRecords", number_text(health.emitted_records, buffer));
    output.add("Dropped", number_text(health.dropped, buffer));
    output.add("Omitted", number_text(health.omitted, buffer));
    output.add("EtwEventsLost", number_text(health.etw_events_lost, buffer));
    output.add("EtwBuffersLost", number_text(health.etw_buffers_lost, buffer));
    output.add("RingHighWater", number_text(health.high_water, buffer));
    output.add("UnexpectedThreadRecords", number_text(health.unexpected_thread_records, buffer));
    output.add("CoreMigrations", number_text(health.core_migrations, buffer));
    output.add("WriterErrors", number_text(health.writer_errors, buffer));
    if (health.file_limit_reached) {
        output.add("State", "File limit reached");
    } else if (health.writer_errors != 0) {
        output.add("State", "Writer error");
    } else {
        output.add("State", "Recording");
    }
}

CaptureMode NetworkDiagnostics::capture_mode() const noexcept {
    return settings_.capture;
}

trace::Recorder& NetworkDiagnostics::recorder() noexcept {
    return recorder_;
}

void NetworkDiagnostics::observe_group(int destination, bool begin) noexcept {
    if (begin) {
        gGroup = {
            .owner = this,
            .start = trace::read_stamp(),
            .record = {.destination = destination},
            .active = true,
        };
        return;
    }
    if (!gGroup.active || gGroup.owner != this) {
        recorder_.omit();
        return;
    }

    gGroup.record.duration = trace::read_stamp().timestamp - gGroup.start.timestamp;
    std::uint16_t flags = trace::RecordFlags::End;
    if (gGroup.record.failed_packets != 0) {
        flags |= trace::RecordFlags::Anomaly;
    }
    recorder_.submit(trace::RecordKind::NativeSendGroup, trace::payload_bytes(gGroup.record),
                     static_cast<std::uint32_t>(destination), flags);
    gGroup.active = false;
}

void NetworkDiagnostics::observe_send(int destination, std::size_t bytes, network_pipeline::PacketCarrier carrier,
                                      int result) noexcept {
    if (gGroup.active && gGroup.owner == this) {
        ++gGroup.record.packets;
        gGroup.record.bytes += static_cast<std::uint32_t>(
            (std::min)(bytes, static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
        if (carrier == network_pipeline::PacketCarrier::Direct) {
            ++gGroup.record.direct_packets;
        } else if (carrier == network_pipeline::PacketCarrier::Native) {
            ++gGroup.record.native_packets;
        }
        gGroup.record.failed_packets += result < 0 ? 1U : 0U;
        return;
    }

    const trace::NativePacketRecord record{
        .destination = destination,
        .bytes = static_cast<std::uint32_t>(
            (std::min)(bytes, static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()))),
        .result = result,
    };
    const auto flags = result < 0 ? trace::RecordFlags::Anomaly : trace::RecordFlags::None;
    recorder_.submit(trace::RecordKind::NativePacket, trace::payload_bytes(record),
                     static_cast<std::uint32_t>(destination), flags, trace_carrier(carrier));
}

void NetworkDiagnostics::observe_receive(bool begin) noexcept {
    if (begin) {
        gReceive = {.owner = this, .start = trace::read_stamp(), .active = true};
        return;
    }
    if (!gReceive.active || gReceive.owner != this) {
        recorder_.omit();
        return;
    }

    // Empty receive-service polls carry no packet evidence and can occur hundreds of thousands of times during loads.
    if (gReceive.record.native_packets == 0 && gReceive.record.direct_packets == 0 &&
        gReceive.record.direct_data_packets == 0) {
        gReceive.active = false;
        return;
    }

    gReceive.record.duration = trace::read_stamp().timestamp - gReceive.start.timestamp;
    recorder_.submit(trace::RecordKind::TransportReceivePass, trace::payload_bytes(gReceive.record), 0,
                     trace::RecordFlags::End);
    gReceive.active = false;
}

void NetworkDiagnostics::observe_intake(void* endpoint, bool direct) noexcept {
    if (gReceive.active && gReceive.owner == this) {
        if (direct) {
            ++gReceive.record.direct_packets;
        } else {
            ++gReceive.record.native_packets;
        }
        return;
    }

    const trace::NativePacketRecord record{
        .endpoint = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(endpoint)),
        .destination = -1,
    };
    recorder_.submit(trace::RecordKind::NativePacket, trace::payload_bytes(record), record.endpoint,
                     trace::RecordFlags::None, direct ? trace::Carrier::Direct : trace::Carrier::Native);
}

void NetworkDiagnostics::observe_disconnect(int destination, bool begin) noexcept {
    const trace::ConnectionRecord record{.destination = destination, .operation = begin ? 1U : 2U};
    recorder_.submit(trace::RecordKind::Connection, trace::payload_bytes(record),
                     static_cast<std::uint32_t>(destination),
                     begin ? trace::RecordFlags::Begin : trace::RecordFlags::End);
}

void NetworkDiagnostics::observe_reset(std::uint8_t mode) noexcept {
    const trace::ConnectionRecord record{.destination = -1, .operation = static_cast<std::uint32_t>(mode) + 3U};
    recorder_.submit(trace::RecordKind::Connection, trace::payload_bytes(record));
}

void NetworkDiagnostics::observe_direct_association(
    const network_pipeline::DirectAssociationObservation& observation) noexcept {
    const trace::DirectAssociationRecord record{
        .slot = observation.slot,
        .generation = observation.generation,
        .connection_id = observation.connection_id,
        .phase = direct_phase(observation.phase),
        .route = direct_route(observation.route),
        .route_reason = observation.route_reason,
        .end_reason = observation.end_reason,
        .attempts = observation.attempts,
        .tx_datagrams = observation.tx_datagrams,
        .rx_datagrams = observation.rx_datagrams,
        .send_failures = observation.send_failures,
        .endpoint_rejects = observation.endpoint_rejects,
        .authentication_rejects = observation.authentication_rejects,
        .replay_rejects = observation.replay_rejects,
        .invalid_rejects = observation.invalid_rejects,
        .elapsed_ms = observation.elapsed_ms,
        .direct_ms = observation.direct_ms,
    };
    std::uint16_t flags{};
    if (observation.phase == network_pipeline::DirectAssociationPhase::Started) {
        flags = trace::RecordFlags::Begin;
    } else if (observation.phase == network_pipeline::DirectAssociationPhase::Ended) {
        flags = trace::RecordFlags::End;
    }
    recorder_.submit(trace::RecordKind::DirectAssociation, trace::payload_bytes(record),
                     static_cast<std::uint32_t>(observation.slot), flags, association_carrier(record.route));
}

void NetworkDiagnostics::observe_direct_receive(
    const network_pipeline::DirectReceiveObservation& observation) noexcept {
    if (!gReceive.active || gReceive.owner != this) {
        recorder_.omit();
        return;
    }
    auto& record = gReceive.record;
    if (record.direct_data_packets++ == 0) {
        record.direct_slot = observation.slot;
        record.direct_generation = observation.generation;
        record.direct_connection_id = observation.connection_id;
        record.first_direct_sequence = observation.sequence;
    } else if (record.direct_slot != observation.slot || record.direct_generation != observation.generation ||
               record.direct_connection_id != observation.connection_id) {
        record.direct_slot = -2;
        record.direct_generation = 0;
        record.direct_connection_id = 0;
    }
    record.last_direct_sequence = observation.sequence;
    if (observation.slot >= 0 && observation.slot < 64) {
        record.direct_player_mask |= std::uint64_t{1} << static_cast<std::uint32_t>(observation.slot);
    }
}

void NetworkDiagnostics::observe_output_pacing(const network_pipeline::OutputPacingObservation& observation) noexcept {
    const trace::OutputPacingRecord record{
        .slot = observation.slot,
        .generation = observation.generation,
        .outcome = pacing_outcome(observation.outcome),
        .fragment_count = observation.fragment_count,
        .bytes = observation.bytes,
        .completion_ns = observation.completion_ns,
        .release_ns = observation.release_ns,
    };
    auto flags = trace::RecordFlags::None;
    if (observation.outcome == network_pipeline::OutputPacingOutcome::QueueCollision ||
        observation.outcome == network_pipeline::OutputPacingOutcome::LifecycleDiscard ||
        observation.outcome == network_pipeline::OutputPacingOutcome::CapacityExceeded) {
        flags = trace::RecordFlags::Anomaly;
    }
    recorder_.submit(trace::RecordKind::OutputPacing, trace::payload_bytes(record),
                     static_cast<std::uint32_t>(observation.slot), flags, trace::Carrier::Direct);
}

} // namespace fusioncutter::patches::network_diagnostics
