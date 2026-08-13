#pragma once

#include "endpoint.hpp"
#include "../shared/diagnostics.hpp"
#include "../shared/galaxy.hpp"
#include "../shared/game_layout.hpp"
#include "../shared/game_transport.hpp"
#include "../shared/native_packet.hpp"
#include "../shared/protocol.hpp"
#include "../shared/security.hpp"
#include "../shared/socket.hpp"
#include "../shared/thread_affinity.hpp"

#include <FusionCutter/outcome.hpp>
#include <FusionCutter/reporting.hpp>
#include <FusionCutter/target.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace fusioncutter::patches::direct_transport::server {

// Maintains one immutable carrier decision for each connected player generation.
class ServerTransport final : public GameTransport, public EndpointSink {
  public:
    ServerTransport(ImageContext image, const GameLayout& game_layout) noexcept;
    ~ServerTransport() override;

    // Validates the native player-removal operations required by enforced policies.
    void build_plan(PatchPlan& plan) const;
    // Resolves native helpers and binds direct UDP to the game's configured port.
    [[nodiscard]] std::expected<void, OutcomeReason> prepare(Policy policy);
    void shutdown() noexcept;
    void write_status(StatusSection& output) const noexcept;

    // Pumps control and direct input around the game's native receive pass.
    void before_receive() noexcept override;
    void after_receive() noexcept override;
    // Discovers or refreshes a player association from native packet activity.
    void on_native_transmit(int physical_primary) noexcept override;
    // Pins one carrier across each nested native transmit group.
    [[nodiscard]] int begin_transmit_group(int physical_primary, int packet_type) noexcept override;
    void end_transmit_group(int physical_primary) noexcept override;
    // Sends a native packet through Direct when the pinned route requires it.
    [[nodiscard]] NativeTransmitResult transmit_native(int physical_primary, int group_primary,
                                                       std::span<const std::uint8_t> bytes) noexcept override;
    // Uses native intake as the second path for discovering player associations.
    void on_native_intake(void* endpoint) noexcept override;
    // Ends or rearms associations as their native connection and lobby lifecycles change.
    void on_native_disconnect(int physical_primary) noexcept override;
    void on_native_disconnect_complete(int physical_primary) noexcept override;
    void on_reset(std::uint8_t mode) noexcept override;
    void on_remote_member(const void* member_id, std::uint32_t state) noexcept override;
    void on_local_lobby_left() noexcept override;
    // Receives the public server address discovered by the late Galaxy observer.
    void observe_public_endpoint(std::uint32_t ipv4_network_order) noexcept override;
    void observe_unusable_endpoint() noexcept override;

  private:
    enum class RemovalReason : std::uint8_t {
        ControlLimit,
        ProtocolVersion,
        DirectRequired,
        NegotiationFailed,
        Count,
    };

    // Holds one physical player's negotiation generation and carrier state.
    struct Association {
        std::uint64_t galaxy_id{};
        std::uint64_t client_nonce{};
        std::uint64_t phase_start_ms{};
        std::uint64_t proof_deadline_ms{};

        std::uint32_t connection_id{};
        std::uint32_t generation{};
        std::uint32_t send_sequence{};

        SessionKey session_key{};
        Endpoint provisional_endpoint{};
        Endpoint committed_endpoint{};
        ReplayWindow receive_replay;

        RouteState state{RouteState::Unclassified};
        TransmitRoute transmit_route{TransmitRoute::Galaxy};
        TransmitGroupState transmit_group;
        AssociationDiagnostics diagnostics;

        std::uint8_t offer_attempts{};
        std::uint8_t commit_attempts{};
        std::uint8_t control_messages_seen{};

        bool live{};
        bool offer_submitted{};
        bool receive_permission{};
    };

    // Accesses the game's Galaxy packet interface for the control channel.
    [[nodiscard]] GalaxyNetworking networking() const noexcept;
    // Maps game destinations, native endpoints, and protocol IDs to physical player slots.
    [[nodiscard]] bool read_identity(std::uint8_t physical_primary, std::uint64_t& identity) const noexcept;
    [[nodiscard]] int resolve_primary(int destination) const noexcept;
    [[nodiscard]] int find_identity(std::uint64_t identity) const noexcept;
    [[nodiscard]] int find_endpoint(const void* endpoint) const noexcept;
    [[nodiscard]] int find_connection(std::uint32_t connection_id) const noexcept;

    // Starts or ends one generation of negotiation for a physical player slot.
    void start_association(std::uint8_t physical_primary, std::uint64_t identity, std::uint64_t now) noexcept;
    void invalidate_association(std::uint8_t physical_primary,
                                AssociationEndReason reason = AssociationEndReason::Disconnected) noexcept;
    void invalidate_all(AssociationEndReason reason = AssociationEndReason::Disconnected) noexcept;
    void observe_association(std::uint8_t physical_primary, network_pipeline::DirectAssociationPhase phase,
                             std::uint64_t now, AssociationEndReason end_reason = AssociationEndReason::Disconnected,
                             std::uint32_t attempts = 0) noexcept;
    // Drains the bounded Galaxy control channel and direct UDP socket.
    void pump_control(std::uint64_t now) noexcept;
    void pump_direct() noexcept;
    // Advances handshake retries, deadlines, carrier selection, and pending removals.
    void service_associations(std::uint64_t now) noexcept;
    // Applies one control message to the matching player negotiation.
    void handle_control(std::uint8_t physical_primary, const ParsedControl& control, ParseStatus status,
                        std::uint64_t now) noexcept;
    // Authenticates, replay-checks, and delivers one recognized UDP datagram.
    void handle_direct(std::span<const std::uint8_t> bytes, const Endpoint& source) noexcept;
    // Creates the security material and offer for one negotiating player.
    void begin_offer(std::uint8_t physical_primary, std::uint64_t nonce, std::uint64_t now) noexcept;
    // Sends the server side of the Galaxy and UDP handshakes.
    [[nodiscard]] bool send_control(std::uint8_t physical_primary, std::span<const std::uint8_t> bytes) noexcept;
    void send_probe_ack(std::uint8_t physical_primary, const Endpoint& destination) noexcept;
    // Applies the configured fallback or removal policy after negotiation fails.
    void direct_failure(std::uint8_t physical_primary, RouteReason reason) noexcept;
    // Completes requested player removals through the game's native update path.
    void service_removals() noexcept;
    void remove_peer(std::uint8_t physical_primary, RemovalReason reason) noexcept;
    // Publishes per-player carrier decisions and their aggregate status counts.
    void set_route(std::uint8_t physical_primary, RouteState state, TransmitRoute route, bool receive_permission,
                   RouteReason reason = RouteReason::None) noexcept;
    void publish_route_counts() noexcept;
    [[nodiscard]] bool direct_route_ready(std::uint8_t physical_primary) const noexcept;
    // Sends one native fragment with Direct sequence and authentication assigned at this call.
    [[nodiscard]] NativeTransmitResult send_direct_packet(std::uint8_t physical_primary,
                                                          std::span<const std::uint8_t> bytes) noexcept;
    static void send_paced_packet(void* context, std::uint8_t physical_primary,
                                  std::span<const std::uint8_t> bytes) noexcept;
    // Rejects callbacks that arrive outside the claimed game network thread.
    [[nodiscard]] bool claim_network_thread(std::string_view operation) noexcept;
    [[nodiscard]] bool on_network_thread(std::string_view operation) noexcept;

    ImageContext image_;
    const GameLayout& game_layout_;
    NativePacketFactory packet_factory_;
    Policy policy_{Policy::Disabled};
    std::uint16_t game_port_{};
    std::array<Association, kPhysicalAssociationCount> associations_{};
    std::array<bool, kPhysicalAssociationCount> rearm_blocked_{};
    std::array<int, kPhysicalAssociationCount> pending_removals_{};
    bool has_pending_removals_{};
    std::uint32_t next_generation_{};
    NetworkThreadAffinity network_thread_;
    std::uint64_t next_control_pump_ms_{};
    std::array<std::uint8_t, 64> control_buffer_{};
    UdpSocketRuntime socket_;
    std::array<std::uint8_t, kMaximumDirectDatagramBytes> receive_buffer_{};
    std::array<std::uint8_t, kMaximumDirectDatagramBytes> send_buffer_{};
    RuntimeLog runtime_log_;
    std::array<std::uint64_t, std::to_underlying(RemovalReason::Count)> removal_counts_{};
    std::atomic_uint32_t public_ipv4_{};
    std::atomic_bool endpoint_unavailable_{};
    std::atomic_uint16_t direct_count_{};
    std::atomic_uint16_t galaxy_count_{};
    std::atomic_uint16_t negotiating_count_{};
    std::atomic_bool socket_available_{};
};

} // namespace fusioncutter::patches::direct_transport::server
