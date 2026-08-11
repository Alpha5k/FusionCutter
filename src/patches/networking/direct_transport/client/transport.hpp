#pragma once

#include "../shared/galaxy.hpp"
#include "../shared/game_layout.hpp"
#include "../shared/game_transport.hpp"
#include "../shared/diagnostics.hpp"
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

namespace fusioncutter::patches::direct_transport::client {

// Maintains the one client-to-host association and moves native packets between Galaxy and authenticated UDP.
class ClientTransport final : public GameTransport {
  public:
    ClientTransport(ImageContext image, const GameLayout& layout) noexcept;
    ~ClientTransport();

    // Resolves the native packet helpers and opens an ephemeral UDP socket.
    [[nodiscard]] std::expected<void, OutcomeReason> prepare();
    void shutdown() noexcept;
    void write_status(StatusSection& output) const noexcept;

    // Pumps control and direct input around the game's native receive pass.
    void before_receive() noexcept override;
    void after_receive() noexcept override;
    // Discovers the current host association from native packet activity.
    void on_native_transmit(int physical_primary) noexcept override;
    // Pins one carrier across each nested native transmit group.
    [[nodiscard]] int begin_transmit_group(int physical_primary) noexcept override;
    void end_transmit_group(int physical_primary) noexcept override;
    // Sends a native packet through Direct when the pinned route requires it.
    [[nodiscard]] NativeTransmitResult transmit_native(int physical_primary, int group_primary,
                                                       std::span<const std::uint8_t> bytes) noexcept override;
    // Uses native intake as the second path for discovering the current host.
    void on_native_intake(void* endpoint) noexcept override;
    // Ends the association when its native connection or lobby lifecycle ends.
    void on_native_disconnect(int physical_primary) noexcept override;
    void on_reset(std::uint8_t mode) noexcept override;
    void on_remote_member(const void* member_id, std::uint32_t state) noexcept override;
    void on_local_lobby_left() noexcept override;

  private:
    static constexpr std::uint8_t kHostPrimary = kPhysicalAssociationCount;

    // Holds one generation of client-to-host negotiation and carrier state.
    struct Association {
        std::uint64_t lobby_id{};
        std::uint64_t owner_user_id{};
        std::uint64_t client_nonce{};
        std::uint64_t phase_start_ms{};
        std::uint64_t proof_start_ms{};

        std::uint32_t connection_id{};
        std::uint32_t generation{};
        std::uint32_t send_sequence{};

        SessionKey session_key{};
        Endpoint server_endpoint{};
        ReplayWindow receive_replay;

        RouteState state{RouteState::Unclassified};
        TransmitRoute transmit_route{TransmitRoute::Galaxy};
        TransmitGroupState transmit_group;
        AssociationDiagnostics diagnostics;

        std::uint8_t caps_attempts{};
        std::uint8_t probe_attempts{};
        std::uint8_t ready_attempts{};
        std::uint8_t control_messages_seen{};

        bool live{};
        bool caps_submitted{};
        bool ready_submitted{};
        bool receive_permission{};
    };

    // Reads the Galaxy interfaces and game-owned identities for the current lobby host.
    [[nodiscard]] GalaxyNetworking networking() const noexcept;
    [[nodiscard]] bool read_lobby_owner(std::uint64_t lobby_id, std::uint64_t& owner_user_id) const noexcept;
    [[nodiscard]] bool read_identity(std::uint8_t physical_primary, std::uint64_t& identity) const noexcept;
    [[nodiscard]] bool current_host(std::uint64_t identity) const noexcept;
    // Starts or ends one generation of client-to-host negotiation.
    void start_association(std::uint64_t lobby_id, std::uint64_t owner_user_id, std::uint64_t now) noexcept;
    void invalidate_association(AssociationEndReason reason = AssociationEndReason::Disconnected) noexcept;
    // Drains the bounded Galaxy control channel and direct UDP socket.
    void pump_control(std::uint64_t now) noexcept;
    void pump_direct(std::uint64_t now) noexcept;
    // Advances handshake retries, deadlines, and carrier selection.
    void service_association(std::uint64_t now) noexcept;
    // Applies one control message to the client negotiation state machine.
    void handle_control(const ParsedControl& control, ParseStatus status, std::uint64_t now) noexcept;
    // Authenticates, replay-checks, and delivers one recognized UDP datagram.
    void handle_direct(std::span<const std::uint8_t> bytes, const Endpoint& source, std::uint64_t now) noexcept;
    // Sends the client side of the Galaxy and UDP handshakes.
    [[nodiscard]] bool send_control(std::span<const std::uint8_t> bytes) noexcept;
    void send_probe() noexcept;
    // Publishes the association's current or terminal carrier decision.
    void lock_galaxy(RouteReason reason) noexcept;
    void set_route(RouteState state, TransmitRoute route, bool receive_permission,
                   RouteReason reason = RouteReason::None) noexcept;
    void publish_status() noexcept;
    // Rejects callbacks that arrive outside the claimed game network thread.
    [[nodiscard]] bool claim_network_thread(std::string_view operation) noexcept;
    [[nodiscard]] bool on_network_thread(std::string_view operation) noexcept;

    ImageContext image_;
    const GameLayout& layout_;
    Association association_;
    NativePacketFactory packet_factory_;
    std::uint32_t next_generation_{};
    NetworkThreadAffinity network_thread_;
    std::uint64_t next_control_pump_ms_{};
    std::array<std::uint8_t, 64> control_buffer_{};
    UdpSocketRuntime socket_;
    std::array<std::uint8_t, kMaximumDirectDatagramBytes> receive_buffer_{};
    std::array<std::uint8_t, kMaximumDirectDatagramBytes> send_buffer_{};
    RuntimeLog runtime_log_;
    std::atomic<RouteState> published_route_{RouteState::Unclassified};
    std::atomic<RouteReason> published_reason_{RouteReason::None};
    std::atomic_uint64_t published_endpoint_{};
    std::atomic_uint16_t published_port_{};
    std::atomic_bool socket_available_{};
};

} // namespace fusioncutter::patches::direct_transport::client
