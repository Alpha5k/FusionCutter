#include "transport.hpp"

#include "../../network_pipeline/pipeline.hpp"

#include "../shared/datagram.hpp"

#include <WinSock2.h>
#include <Windows.h>
#include <array>

namespace fusioncutter::patches::direct_transport::server {
namespace {

constexpr std::uint64_t kOfferSafetyTimeoutMs = 10'000;

} // namespace

void ServerTransport::start_association(std::uint8_t physical_primary, std::uint64_t identity,
                                        std::uint64_t now) noexcept {
    Association next{};
    next.transmit_group = associations_[physical_primary].transmit_group;
    next.live = true;
    next.galaxy_id = identity;
    next.phase_start_ms = now;
    if (++next_generation_ == 0) {
        ++next_generation_;
    }
    next.generation = next_generation_;
    next.diagnostics.reset(now);
    associations_[physical_primary] = next;
    observe_association(physical_primary, network_pipeline::DirectAssociationPhase::Started, now);
    publish_route_counts();
}

void ServerTransport::invalidate_association(std::uint8_t physical_primary, AssociationEndReason reason) noexcept {
    if (physical_primary >= associations_.size() || !associations_[physical_primary].live) {
        return;
    }
    auto& association = associations_[physical_primary];
    const auto now = GetTickCount64();
    observe_association(physical_primary, network_pipeline::DirectAssociationPhase::Ended, now, reason);
    association.diagnostics.finish("Server", physical_primary, association.generation, association.connection_id,
                                   association.state, reason, now);
    const auto transmit_group = association.transmit_group;
    association = {};
    association.transmit_group = transmit_group;
    publish_route_counts();
}

void ServerTransport::observe_association(std::uint8_t physical_primary, network_pipeline::DirectAssociationPhase phase,
                                          std::uint64_t now, AssociationEndReason end_reason,
                                          std::uint32_t attempts) noexcept {
    const auto& association = associations_[physical_primary];
    const auto snapshot = association.diagnostics.snapshot(now);
    network_pipeline::observe_direct_association({
        .slot = physical_primary,
        .generation = association.generation,
        .connection_id = association.connection_id,
        .phase = phase,
        .route = static_cast<std::uint8_t>(association.state),
        .route_reason = static_cast<std::uint8_t>(association.diagnostics.route_reason()),
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

void ServerTransport::invalidate_all(AssociationEndReason reason) noexcept {
    for (std::uint8_t physical_primary = 0; physical_primary < associations_.size(); ++physical_primary) {
        invalidate_association(physical_primary, reason);
    }
}

void ServerTransport::pump_control(std::uint64_t now) noexcept {
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
        const auto physical_primary = find_identity(sender);
        if (physical_primary < 0 || !associations_[physical_primary].live ||
            associations_[physical_primary].galaxy_id != sender) {
            continue;
        }
        ParsedControl control{};
        const auto status = parse_control(std::span<const std::uint8_t>(control_buffer_).first(bytes_read), control);
        handle_control(static_cast<std::uint8_t>(physical_primary), control, status, now);
    }
}

void ServerTransport::pump_direct() noexcept {
    if (!socket_.available()) {
        return;
    }
    const auto drained = drain_datagrams(
        [this](Endpoint& source) noexcept {
            return socket_.receive(receive_buffer_, source);
        },
        [this](std::int32_t bytes, const Endpoint& source) noexcept {
            handle_direct(std::span<const std::uint8_t>(receive_buffer_).first(bytes), source);
        });
    if (drained.error != 0) {
        runtime_log_.socket("Receive direct UDP datagram", drained.error, is_internal_socket_error(drained.error));
    }
    runtime_log_.receive_overflow(drained.discarded);
}

void ServerTransport::handle_control(std::uint8_t physical_primary, const ParsedControl& control, ParseStatus status,
                                     std::uint64_t now) noexcept {
    auto& association = associations_[physical_primary];
    if (++association.control_messages_seen > kControlAssociationLimit) {
        remove_peer(physical_primary, RemovalReason::ControlLimit);
        return;
    }
    if (status == ParseStatus::UnsupportedVersion && control.kind == ControlKind::Caps && control.version != 0 &&
        association.state == RouteState::Unclassified) {
        if (policy_ == Policy::PreferDirect) {
            set_route(physical_primary, RouteState::GalaxyLocked, TransmitRoute::Galaxy, false,
                      RouteReason::ProtocolIncompatible);
        } else {
            remove_peer(physical_primary, RemovalReason::ProtocolVersion);
        }
        return;
    }
    if (status != ParseStatus::Ok) {
        association.diagnostics.reject(RejectionKind::Invalid);
        return;
    }
    if (control.kind == ControlKind::Caps && association.state == RouteState::Unclassified) {
        begin_offer(physical_primary, control.client_nonce, now);
    } else if (control.kind == ControlKind::Ready && association.state == RouteState::Negotiating &&
               association.offer_submitted && association.provisional_endpoint.valid() &&
               control.connection_id == association.connection_id) {
        association.commit_attempts = 0;
        association.phase_start_ms = now;
        set_route(physical_primary, RouteState::AwaitCommit, association.transmit_route,
                  association.receive_permission);
    }
}

void ServerTransport::handle_direct(std::span<const std::uint8_t> bytes, const Endpoint& source) noexcept {
    ParsedDirect direct{};
    if (parse_direct(bytes, direct) != ParseStatus::Ok) {
        return;
    }
    const auto found = find_connection(direct.connection_id);
    if (found < 0) {
        return;
    }
    const auto physical_primary = static_cast<std::uint8_t>(found);
    auto& association = associations_[physical_primary];
    const auto valid_state = direct.kind == DirectKind::Probe
                                 ? association.offer_submitted && (association.state == RouteState::Negotiating ||
                                                                   association.state == RouteState::AwaitCommit)
                                 : direct.kind == DirectKind::Data && association.receive_permission &&
                                       association.state == RouteState::DirectLocked;
    if (!valid_state) {
        association.diagnostics.reject(RejectionKind::Invalid);
        return;
    }
    const auto expected_endpoint =
        direct.kind == DirectKind::Data ? association.committed_endpoint : association.provisional_endpoint;
    if (!endpoint_admits_source(expected_endpoint, source, direct.kind == DirectKind::Probe)) {
        association.diagnostics.reject(RejectionKind::Endpoint);
        return;
    }
    if (!verify_authentication_tag(Direction::ClientToServer, association.session_key, bytes.first(kDirectHeaderBytes),
                                   direct.payload)) {
        association.diagnostics.reject(RejectionKind::Authentication);
        return;
    }
    const auto replay = association.receive_replay.admit(direct.datagram_sequence);
    if (replay == ReplayResult::Duplicate || replay == ReplayResult::Stale || replay == ReplayResult::InvalidJump) {
        association.diagnostics.reject(RejectionKind::Replay);
        return;
    }
    if (direct.kind == DirectKind::Probe) {
        if (!association.provisional_endpoint.valid()) {
            association.provisional_endpoint = source;
        }
        send_probe_ack(physical_primary, source);
        return;
    }

    void* packet{};
    const auto built = build_native_packet(packet_factory_, direct.payload, packet);
    if (built == NativePacketResult::Built) {
        network_pipeline::submit_native(packet, &association.galaxy_id);
        association.diagnostics.direct_rx("Server", physical_primary, association.generation,
                                          association.connection_id);
        network_pipeline::observe_direct_receive({
            .slot = physical_primary,
            .generation = association.generation,
            .connection_id = association.connection_id,
            .sequence = direct.datagram_sequence,
        });
    } else if (built == NativePacketResult::RejectedPayload) {
        association.diagnostics.reject(RejectionKind::Invalid);
    } else if (built == NativePacketResult::FactoryUnavailable) {
        runtime_log_.error("The native packet factory is unavailable", "Resolve native packet factory");
    } else if (built == NativePacketResult::InvalidPrefix) {
        runtime_log_.error("The native packet prefix is invalid", "Validate native packet prefix");
    } else {
        runtime_log_.error("The game could not allocate a native packet", "Allocate received native packet");
    }
}

void ServerTransport::begin_offer(std::uint8_t physical_primary, std::uint64_t nonce, std::uint64_t now) noexcept {
    auto& association = associations_[physical_primary];
    association.client_nonce = nonce;
    association.phase_start_ms = now;
    association.offer_attempts = 0;
    set_route(physical_primary, RouteState::Negotiating, association.transmit_route, association.receive_permission);

    if (!socket_.available() || game_port_ == 0) {
        direct_failure(physical_primary, RouteReason::SocketUnavailable);
        return;
    }
    if (public_ipv4_.load(std::memory_order_acquire) == 0) {
        direct_failure(physical_primary, RouteReason::EndpointUnavailable);
        return;
    }
    std::array<std::uint32_t, kPhysicalAssociationCount> live_ids{};
    for (std::size_t index = 0; index < associations_.size(); ++index) {
        if (associations_[index].live) {
            live_ids[index] = associations_[index].connection_id;
        }
    }
    if (!generate_connection_id(system_random_source(), live_ids, association.connection_id) ||
        !fill_random(system_random_source(), association.session_key)) {
        runtime_log_.error("Secure random generation failed", "Generate server connection security");
        direct_failure(physical_primary, RouteReason::RandomGenerationFailed);
    }
}

bool ServerTransport::send_control(std::uint8_t physical_primary, std::span<const std::uint8_t> bytes) noexcept {
    return networking().send_reliable_immediate(associations_[physical_primary].galaxy_id, bytes, kControlChannel);
}

void ServerTransport::send_probe_ack(std::uint8_t physical_primary, const Endpoint& destination) noexcept {
    auto& association = associations_[physical_primary];
    std::array<std::uint8_t, kProbeDatagramBytes> datagram{};
    static_cast<void>(write_probe_datagram(datagram, DirectKind::ProbeAck, Direction::ServerToClient,
                                           association.session_key, association.connection_id,
                                           association.send_sequence++));
    const auto sent = socket_.send(datagram, destination);
    if (!sent.succeeded()) {
        association.diagnostics.send_failed();
        runtime_log_.socket("Send direct UDP probe acknowledgement", sent.error, is_internal_socket_error(sent.error));
    }
}

void ServerTransport::service_associations(std::uint64_t now) noexcept {
    for (std::uint8_t physical_primary = 0; physical_primary < associations_.size(); ++physical_primary) {
        auto& association = associations_[physical_primary];
        if (!association.live) {
            continue;
        }
        std::uint64_t current{};
        if (!read_identity(physical_primary, current) || current != association.galaxy_id) {
            invalidate_association(physical_primary, AssociationEndReason::Disconnected);
            continue;
        }
        if (association.state == RouteState::Unclassified) {
            if (now - association.phase_start_ms >= kHandshakeDeadlineMs) {
                if (policy_ == Policy::RequireDirectAll) {
                    remove_peer(physical_primary, RemovalReason::DirectRequired);
                } else {
                    set_route(physical_primary, RouteState::GalaxyLocked, TransmitRoute::Galaxy, false,
                              RouteReason::CapabilityTimeout);
                }
            }
            continue;
        }
        if (association.state == RouteState::Negotiating) {
            if (association.connection_id == 0) {
                continue;
            }
            if (!association.offer_submitted) {
                if (now - association.phase_start_ms >= kHandshakeDeadlineMs) {
                    direct_failure(physical_primary, RouteReason::ProofTimeout);
                } else if (association.offer_attempts < kHandshakeAttemptLimit &&
                           now - association.phase_start_ms >= association.offer_attempts * kHandshakeRetryIntervalMs) {
                    std::array<std::uint8_t, kOfferBytes> offer{};
                    static_cast<void>(write_offer(offer, association.client_nonce, association.connection_id,
                                                  public_ipv4_.load(std::memory_order_acquire), game_port_,
                                                  association.session_key));
                    ++association.offer_attempts;
                    association.offer_submitted = send_control(physical_primary, offer);
                    if (association.offer_submitted) {
                        association.proof_deadline_ms = now + kOfferSafetyTimeoutMs;
                    }
                }
            } else if (now >= association.proof_deadline_ms) {
                direct_failure(physical_primary, association.provisional_endpoint.valid() ? RouteReason::ReadyTimeout
                                                                                          : RouteReason::ProofTimeout);
            }
            continue;
        }
        if (association.state == RouteState::AwaitCommit) {
            if (now - association.phase_start_ms >= kHandshakeDeadlineMs) {
                direct_failure(physical_primary, RouteReason::CommitTimeout);
            } else if (association.commit_attempts < kHandshakeAttemptLimit &&
                       now - association.phase_start_ms >= association.commit_attempts * kHandshakeRetryIntervalMs) {
                std::array<std::uint8_t, kActivationBytes> commit{};
                static_cast<void>(write_activation(commit, ControlKind::Commit, association.connection_id));
                ++association.commit_attempts;
                if (send_control(physical_primary, commit)) {
                    association.committed_endpoint = association.provisional_endpoint;
                    set_route(physical_primary, RouteState::DirectLocked, TransmitRoute::DirectPending, true,
                              RouteReason::Commit);
                }
            }
        }
    }
    service_removals();
}

void ServerTransport::direct_failure(std::uint8_t physical_primary, RouteReason reason) noexcept {
    if (policy_ == Policy::PreferDirect) {
        auto& association = associations_[physical_primary];
        association.provisional_endpoint = {};
        association.committed_endpoint = {};
        set_route(physical_primary, RouteState::GalaxyLocked, TransmitRoute::Galaxy, false, reason);
    } else {
        remove_peer(physical_primary, RemovalReason::NegotiationFailed);
    }
}

void ServerTransport::set_route(std::uint8_t physical_primary, RouteState state, TransmitRoute route,
                                bool receive_permission, RouteReason reason) noexcept {
    auto& association = associations_[physical_primary];
    if (!valid_route_transition(association.state, state)) {
        runtime_log_.error("Direct Transport attempted an invalid route transition",
                           "Validate server route transition");
        return;
    }
    const auto entering_terminal =
        state != association.state && (state == RouteState::GalaxyLocked || state == RouteState::DirectLocked);
    if (entering_terminal && reason == RouteReason::None) {
        runtime_log_.error("A terminal Direct Transport route is missing its reason", "Require server route reason");
        return;
    }
    association.state = state;
    association.transmit_route = route;
    association.receive_permission = receive_permission;
    if (entering_terminal) {
        association.diagnostics.terminal_route("Server", physical_primary, association.generation,
                                               association.connection_id, state, reason, GetTickCount64(),
                                               association.offer_attempts + association.commit_attempts);
        observe_association(physical_primary, network_pipeline::DirectAssociationPhase::Terminal, GetTickCount64(),
                            AssociationEndReason::Disconnected,
                            association.offer_attempts + association.commit_attempts);
    }
    publish_route_counts();
}

void ServerTransport::publish_route_counts() noexcept {
    std::uint16_t direct{};
    std::uint16_t galaxy{};
    std::uint16_t negotiating{};
    for (const auto& association : associations_) {
        if (!association.live) {
            continue;
        }
        switch (association.state) {
        case RouteState::DirectLocked:
            ++direct;
            break;
        case RouteState::GalaxyLocked:
            ++galaxy;
            break;
        case RouteState::Unclassified:
        case RouteState::Negotiating:
        case RouteState::AwaitCommit:
            ++negotiating;
            break;
        }
    }
    direct_count_.store(direct, std::memory_order_release);
    galaxy_count_.store(galaxy, std::memory_order_release);
    negotiating_count_.store(negotiating, std::memory_order_release);
}

} // namespace fusioncutter::patches::direct_transport::server
