#include "transport.hpp"

#include "../../network_pipeline/pipeline.hpp"

#include "../shared/datagram.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <array>

namespace fusioncutter::patches::direct_transport::client {

void ClientTransport::start_association(std::uint64_t lobby_id, std::uint64_t owner_user_id,
                                        std::uint64_t now) noexcept {
    Association next{};
    next.transmit_group = association_.transmit_group;
    next.live = true;
    next.lobby_id = lobby_id;
    next.owner_user_id = owner_user_id;
    next.phase_start_ms = now;
    if (++next_generation_ == 0) {
        ++next_generation_;
    }
    next.generation = next_generation_;
    next.diagnostics.reset(now);
    association_ = next;
    observe_association(network_pipeline::DirectAssociationPhase::Started, now);

    if (!socket_.available()) {
        set_route(RouteState::GalaxyLocked, TransmitRoute::Galaxy, false, RouteReason::SocketUnavailable);
        return;
    }
    std::array<std::uint8_t, 8> nonce{};
    if (!fill_random(system_random_source(), nonce)) {
        runtime_log_.error("Secure random generation failed", "Generate client nonce");
        set_route(RouteState::GalaxyLocked, TransmitRoute::Galaxy, false, RouteReason::RandomGenerationFailed);
        return;
    }
    association_.client_nonce = load_big_endian64(nonce.data());
    publish_status();
}

void ClientTransport::invalidate_association(AssociationEndReason reason) noexcept {
    if (!association_.live) {
        return;
    }
    const auto now = GetTickCount64();
    observe_association(network_pipeline::DirectAssociationPhase::Ended, now, reason);
    association_.diagnostics.finish("Client", kHostPrimary, association_.generation, association_.connection_id,
                                    association_.state, reason, now);
    const auto transmit_group = association_.transmit_group;
    association_ = {};
    association_.transmit_group = transmit_group;
    publish_status();
}

void ClientTransport::observe_association(network_pipeline::DirectAssociationPhase phase, std::uint64_t now,
                                          AssociationEndReason end_reason, std::uint32_t attempts) noexcept {
    const auto snapshot = association_.diagnostics.snapshot(now);
    network_pipeline::observe_direct_association({
        .slot = kHostPrimary,
        .generation = association_.generation,
        .connection_id = association_.connection_id,
        .phase = phase,
        .route = static_cast<std::uint8_t>(association_.state),
        .route_reason = static_cast<std::uint8_t>(association_.diagnostics.route_reason()),
        .end_reason = phase == network_pipeline::DirectAssociationPhase::Ended ? static_cast<std::uint8_t>(end_reason)
                                                                               : std::uint8_t{},
        .attempts = attempts,
        .tx_datagrams = snapshot.tx_datagrams,
        .rx_datagrams = snapshot.rx_datagrams,
        .send_failures = snapshot.send_failures,
        .endpoint_rejects = snapshot.endpoint_rejects,
        .authentication_rejects = snapshot.authentication_rejects,
        .replay_rejects = snapshot.replay_rejects,
        .invalid_rejects = snapshot.invalid_rejects,
        .elapsed_ms = snapshot.elapsed_ms,
        .direct_ms = snapshot.direct_ms,
    });
}

void ClientTransport::pump_control(std::uint64_t now) noexcept {
    if (now < next_control_pump_ms_) {
        return;
    }
    next_control_pump_ms_ = now + kControlPumpIntervalMs;
    const auto network = networking();
    if (!network.valid()) {
        return;
    }
    for (std::uint32_t consumed = 0; consumed < kControlDrainLimit; ++consumed) {
        std::uint32_t reported{};
        if (!network.packet_available(reported, kControlChannel)) {
            break;
        }
        if (reported > control_buffer_.size()) {
            static_cast<void>(network.pop_packet(kControlChannel));
            continue;
        }
        std::uint32_t bytes_read{};
        std::uint64_t sender{};
        if (!network.read_packet(control_buffer_, bytes_read, sender, kControlChannel)) {
            break;
        }
        if (!association_.live || sender != association_.owner_user_id || !current_host(association_.lobby_id)) {
            continue;
        }
        ParsedControl control{};
        const auto status = parse_control(std::span<const std::uint8_t>(control_buffer_).first(bytes_read), control);
        handle_control(control, status, now);
    }
}

void ClientTransport::pump_direct(std::uint64_t now) noexcept {
    if (!socket_.available()) {
        return;
    }
    const auto drained = drain_datagrams(
        [this](Endpoint& source) noexcept {
            return socket_.receive(receive_buffer_, source);
        },
        [this, now](std::int32_t bytes, const Endpoint& source) noexcept {
            handle_direct(std::span<const std::uint8_t>(receive_buffer_).first(bytes), source, now);
        });
    if (drained.error != 0) {
        runtime_log_.socket("Receive direct UDP datagram", drained.error, is_internal_socket_error(drained.error));
    }
    runtime_log_.receive_overflow(drained.discarded);
}

void ClientTransport::handle_control(const ParsedControl& control, ParseStatus status, std::uint64_t now) noexcept {
    if (++association_.control_messages_seen > kControlAssociationLimit) {
        runtime_log_.warning("Host exceeded the Direct Transport control-message limit; disconnecting",
                             "Enforce control message limit");
        association_.diagnostics.mark_policy_action();
        invalidate_association(AssociationEndReason::ControlLimit);
        network_pipeline::disconnect_native(kHostPrimary);
        return;
    }
    if (status == ParseStatus::UnsupportedVersion && control.kind == ControlKind::Offer && control.version != 0 &&
        association_.state == RouteState::Unclassified) {
        lock_galaxy(RouteReason::ProtocolIncompatible);
        return;
    }
    if (status != ParseStatus::Ok) {
        association_.diagnostics.reject(RejectionKind::Invalid);
        return;
    }
    if (control.kind == ControlKind::Commit && association_.state == RouteState::AwaitCommit &&
        control.connection_id == association_.connection_id) {
        set_route(RouteState::DirectLocked, TransmitRoute::DirectPending, association_.receive_permission,
                  RouteReason::Commit);
        return;
    }
    if (control.kind != ControlKind::Offer || association_.state != RouteState::Unclassified ||
        control.client_nonce != association_.client_nonce || control.connection_id == 0 || control.server_port == 0 ||
        !is_public_ipv4(control.server_ipv4_network_order)) {
        association_.diagnostics.reject(RejectionKind::Invalid);
        return;
    }
    association_.connection_id = control.connection_id;
    association_.session_key = control.session_key;
    association_.server_endpoint = {control.server_ipv4_network_order, control.server_port};
    association_.proof_start_ms = now;
    association_.probe_attempts = 0;
    set_route(RouteState::Negotiating, association_.transmit_route, association_.receive_permission);
}

void ClientTransport::handle_direct(std::span<const std::uint8_t> bytes, const Endpoint& source,
                                    std::uint64_t now) noexcept {
    ParsedDirect direct{};
    if (parse_direct(bytes, direct) != ParseStatus::Ok || !association_.live ||
        direct.connection_id != association_.connection_id) {
        return;
    }
    if (source != association_.server_endpoint) {
        association_.diagnostics.reject(RejectionKind::Endpoint);
        return;
    }
    const auto valid_state =
        direct.kind == DirectKind::ProbeAck
            ? association_.state == RouteState::Negotiating || association_.state == RouteState::AwaitCommit
            : direct.kind == DirectKind::Data && association_.receive_permission &&
                  (association_.state == RouteState::AwaitCommit || association_.state == RouteState::DirectLocked);
    if (!valid_state) {
        association_.diagnostics.reject(RejectionKind::Invalid);
        return;
    }
    if (!verify_authentication_tag(Direction::ServerToClient, association_.session_key, bytes.first(kDirectHeaderBytes),
                                   direct.payload)) {
        association_.diagnostics.reject(RejectionKind::Authentication);
        return;
    }
    const auto replay = association_.receive_replay.admit(direct.datagram_sequence);
    if (replay == ReplayResult::Duplicate || replay == ReplayResult::Stale || replay == ReplayResult::InvalidJump) {
        association_.diagnostics.reject(RejectionKind::Replay);
        return;
    }
    if (direct.kind == DirectKind::ProbeAck && association_.state == RouteState::Negotiating) {
        association_.phase_start_ms = now;
        association_.ready_attempts = 0;
        set_route(RouteState::Negotiating, association_.transmit_route, true);
    } else if (direct.kind == DirectKind::Data && association_.state == RouteState::AwaitCommit) {
        set_route(RouteState::DirectLocked, TransmitRoute::DirectPending, true, RouteReason::ImplicitCommit);
    }
    if (direct.kind != DirectKind::Data) {
        return;
    }

    void* packet{};
    const auto built = build_native_packet(packet_factory_, direct.payload, packet);
    if (built == NativePacketResult::Built) {
        network_pipeline::submit_native(packet, &association_.owner_user_id);
        association_.diagnostics.direct_rx("Client", kHostPrimary, association_.generation, association_.connection_id);
        network_pipeline::observe_direct_receive({
            .slot = kHostPrimary,
            .generation = association_.generation,
            .connection_id = association_.connection_id,
            .sequence = direct.datagram_sequence,
        });
    } else if (built == NativePacketResult::RejectedPayload) {
        association_.diagnostics.reject(RejectionKind::Invalid);
    } else if (built == NativePacketResult::FactoryUnavailable) {
        runtime_log_.error("The native packet factory is unavailable", "Resolve native packet factory");
    } else if (built == NativePacketResult::InvalidPrefix) {
        runtime_log_.error("The native packet prefix is invalid", "Validate native packet prefix");
    } else {
        runtime_log_.error("The game could not allocate a native packet", "Allocate received native packet");
    }
}

bool ClientTransport::send_control(std::span<const std::uint8_t> bytes) noexcept {
    return networking().send_reliable_immediate(association_.owner_user_id, bytes, kControlChannel);
}

void ClientTransport::send_probe() noexcept {
    std::array<std::uint8_t, kProbeDatagramBytes> datagram{};
    static_cast<void>(write_probe_datagram(datagram, DirectKind::Probe, Direction::ClientToServer,
                                           association_.session_key, association_.connection_id,
                                           association_.send_sequence++));
    const auto sent = socket_.send(datagram, association_.server_endpoint);
    if (!sent.succeeded()) {
        association_.diagnostics.send_failed();
        runtime_log_.socket("Send direct UDP probe", sent.error, is_internal_socket_error(sent.error));
    }
    ++association_.probe_attempts;
}

void ClientTransport::service_association(std::uint64_t now) noexcept {
    if (!association_.live) {
        return;
    }
    if (!current_host(association_.lobby_id)) {
        invalidate_association(AssociationEndReason::HostChanged);
        return;
    }
    if (association_.state == RouteState::Unclassified) {
        if (now - association_.phase_start_ms >= kHandshakeDeadlineMs) {
            lock_galaxy(RouteReason::CapabilityTimeout);
            return;
        }
        if (!association_.caps_submitted && association_.caps_attempts < kHandshakeAttemptLimit &&
            now - association_.phase_start_ms >= association_.caps_attempts * kHandshakeRetryIntervalMs) {
            std::array<std::uint8_t, kCapsBytes> caps{};
            static_cast<void>(write_caps(caps, association_.client_nonce));
            ++association_.caps_attempts;
            association_.caps_submitted = send_control(caps);
        }
        return;
    }
    if (association_.state == RouteState::AwaitCommit) {
        if (now - association_.phase_start_ms >= kHandshakeDeadlineMs) {
            lock_galaxy(RouteReason::CommitTimeout);
        }
        return;
    }
    if (association_.state != RouteState::Negotiating) {
        return;
    }
    if (!association_.receive_permission) {
        if (now - association_.proof_start_ms >= kHandshakeDeadlineMs) {
            lock_galaxy(RouteReason::ProofTimeout);
        } else if (association_.probe_attempts < kHandshakeAttemptLimit &&
                   now - association_.proof_start_ms >= association_.probe_attempts * kHandshakeRetryIntervalMs) {
            send_probe();
        }
        return;
    }
    if (now - association_.phase_start_ms >= kHandshakeDeadlineMs) {
        lock_galaxy(RouteReason::ReadyTimeout);
        return;
    }
    if (!association_.ready_submitted && association_.ready_attempts < kHandshakeAttemptLimit &&
        now - association_.phase_start_ms >= association_.ready_attempts * kHandshakeRetryIntervalMs) {
        std::array<std::uint8_t, kActivationBytes> ready{};
        static_cast<void>(write_activation(ready, ControlKind::Ready, association_.connection_id));
        ++association_.ready_attempts;
        association_.ready_submitted = send_control(ready);
        if (association_.ready_submitted) {
            set_route(RouteState::AwaitCommit, association_.transmit_route, association_.receive_permission);
        }
    }
}

void ClientTransport::lock_galaxy(RouteReason reason) noexcept {
    set_route(RouteState::GalaxyLocked, TransmitRoute::Galaxy, false, reason);
}

void ClientTransport::set_route(RouteState state, TransmitRoute route, bool receive_permission,
                                RouteReason reason) noexcept {
    if (!valid_route_transition(association_.state, state)) {
        runtime_log_.error("Direct Transport attempted an invalid route transition",
                           "Validate client route transition");
        return;
    }
    const auto entering_terminal =
        state != association_.state && (state == RouteState::GalaxyLocked || state == RouteState::DirectLocked);
    if (entering_terminal && reason == RouteReason::None) {
        runtime_log_.error("A terminal Direct Transport route is missing its reason", "Require client route reason");
        return;
    }
    association_.state = state;
    association_.transmit_route = route;
    association_.receive_permission = receive_permission;
    if (entering_terminal) {
        association_.diagnostics.terminal_route(
            "Client", kHostPrimary, association_.generation, association_.connection_id, state, reason,
            GetTickCount64(), association_.caps_attempts + association_.probe_attempts + association_.ready_attempts);
        observe_association(network_pipeline::DirectAssociationPhase::Terminal, GetTickCount64(),
                            AssociationEndReason::Disconnected,
                            association_.caps_attempts + association_.probe_attempts + association_.ready_attempts);
    }
    publish_status();
}

} // namespace fusioncutter::patches::direct_transport::client
