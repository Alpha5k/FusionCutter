#pragma once

#include "diagnostic.hpp"

#include <fc/game.hpp>
#include <fc/multiplayer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc::runtime {
class MultiplayerService;
struct PatchRecord;
} // namespace fc::runtime

namespace fc::reporting {
class StatusText;
}

// Stores a copied feature definition and callback pointers at a stable runtime address.
struct FC_Feature {
    // Stores a patch-owned callback for peer agreement changes.
    struct PeerCallback {
        void *capture{};
        FC_PeerChange callback{};
    };

    // Stores a patch-owned callback for complete messages.
    struct MessageCallback {
        void *capture{};
        FC_FeatureMessage callback{};
    };

    fc::runtime::MultiplayerService *service{};
    fc::runtime::PatchRecord *owner{};
    std::string id;
    // role contains roles registered during prepare(); frozen contains roles from Installed patches selected at Game Start.
    std::uint32_t role{}, frozen{};
    // Calculated from the copied, validated ID during registration.
    std::uint32_t payload_capacity{};
    std::vector<PeerCallback> peers;
    // Agreement logs and all registered peer callbacks share the last control-thread snapshot.
    std::map<std::uint64_t, fc::Agreement> last;
    std::vector<MessageCallback> messages;
};

namespace fc::runtime {

// Describes whether native transport accepted a complete frame or refused it temporarily or permanently.
enum class CarrierResult {
    Accepted,
    Retry,
    Failed
};

// Supplies frame submission for pump(); keep the adapter context alive until pump() returns.
// submit runs outside the service lock and must copy accepted bytes before returning.
struct Carrier {
    void *context{};
    CarrierResult (*submit)(void *, bool server, std::uint64_t recipient, Bytes) noexcept {};
};

// Tracks native connections, feature agreement and queued messages for the process.
class MultiplayerService final {
  public:
    using Clock = std::chrono::steady_clock;
    MultiplayerService() = default;
    MultiplayerService(const MultiplayerService &) = delete;
    MultiplayerService &operator=(const MultiplayerService &) = delete;

    // Starts a Game session and freezes roles from Installed patches at the actual Game Start.
    void begin(GameState state, bool galaxy_selected);

    // Ends the session, expires peer IDs and discards their queued messages.
    void end();

    // Updates a native slot's address and user association; the same connected slot and raw address preserve its PeerId.
    void connect(std::uint32_t slot, std::uint64_t raw_address, std::optional<std::uint64_t> user_id, bool is_local = false);

    // Expires the peer attached to this slot immediately; reconnecting afterward creates a new PeerId.
    void disconnect(std::uint32_t slot);

    // Clears connections after native Create(false), expiring all current peer IDs.
    void reset_connections();

    // Updates this process's native associations and establishes its host or client ID for the current mode.
    // Keeps existing IDs; a playing host's local client still requires a native connected slot.
    void observe_local(PeerInfo info);

    // Attempts at most sixteen frame submissions; Retry keeps the connection's first queued frame for a later pump.
    // Accepted frames are removed and are not retransmitted.
    void pump(Carrier carrier, Clock::time_point now);

    // Processes an incoming frame; the native adapter calls this at most sixteen times per servicing opportunity.
    void receive(bool server, std::uint64_t sender, Bytes bytes, Clock::time_point now);

    // Fails remote connections when transport cannot preserve accepted delivery or receive a frame.
    void carrier_failed(std::string_view reason);

    // Calls patch callbacks with agreement changes and complete messages, serially on the control thread outside the service lock.
    void deliver();

    // Removes registrations after the handler is destroyed; keep owner alive until this control-thread call returns.
    void discard(PatchRecord &owner);

    // Appends current communication failures to the control-thread report.
    void issues(fc::reporting::StatusText &) const;

    // Returns the current host ID, or zero until native observations establish it.
    PeerId host() const;

    // Returns the local client endpoint; a dedicated host has none and a playing host uses a distinct ID.
    PeerId local() const;

    // Copies connected clients only when hosting, including the local client.
    std::vector<PeerId> clients() const;

    // Copies current native associations; expired IDs return no value.
    std::optional<PeerInfo> info(PeerId peer) const;

    // Registers an exact feature ID and unused roles during prepare(); the returned record keeps a stable address.
    std::expected<FC_Feature *, Diagnostic> feature(PatchRecord &owner, std::string_view id, std::uint32_t role);

    // Registers callbacks during prepare(); the patch keeps captures alive while its handler is Installed.
    std::expected<void, Diagnostic> on_peer(FC_Feature &, void *, FC_PeerChange);
    std::expected<void, Diagnostic> on_message(FC_Feature &, void *, FC_FeatureMessage);

    // Returns agreement for a live peer and an applicable role from the feature list frozen at Game Start.
    Agreement agreement(const FC_Feature &, PeerId peer) const;

    // Copies complete messages without waiting; lock contention and queue exhaustion return Full.
    SendResult send(FC_Feature &, PeerId peer, Bytes bytes) noexcept;

    // Returns the maximum message size, allowing space for the encoded feature ID and frame header.
    std::uint32_t max_payload(const FC_Feature &) const noexcept;

    // Fails this feature's communication with the peer and copies the reason, even when the data queue is full.
    void fail(FC_Feature &, PeerId peer, std::string_view reason);

  private:
    enum class Phase {
        AwaitHello,
        Hello,
        AwaitWelcome,
        Welcome,
        AwaitReady,
        Ready,
        Established,
        Expired,
        Failed
    };

    enum class Rejection : std::uint8_t {
        InvalidPacket = 1,
        WrongRole = 2,
        WrongStage = 4,
        RepeatedHello = 8,
        ClientToken = 16,
        HostToken = 32
    };

    static constexpr std::size_t kQueueBytes = 64 * 1024;

    // Stores outgoing data on its owning connection; its byte budget includes this record and copied payload.
    struct OutgoingMessage {
        FC_Feature *feature{};
        PeerId peer;
        std::vector<std::byte> bytes;

        std::size_t charge() const noexcept {
            return sizeof(OutgoingMessage) + bytes.size();
        }
    };

    // Stores incoming data outside its connection; its byte budget includes this record and copied payload.
    struct IncomingMessage {
        FC_Feature *feature{};
        PeerId peer;
        // Keeps queued data tied to the originating connection when the host endpoint stays unchanged.
        PeerId connection;
        std::vector<std::byte> bytes;

        std::size_t charge() const noexcept {
            return sizeof(IncomingMessage) + bytes.size();
        }
    };

    // Retains a valid Hello until its native sender connects; the incoming byte budget includes this record and its bytes.
    struct PendingHello {
        std::uint64_t sender{};
        Clock::time_point deadline;
        std::vector<std::byte> bytes;

        std::size_t charge() const noexcept {
            return sizeof(PendingHello) + bytes.size();
        }
    };

    // Stores validated remote support and failure flags separately from data queues.
    // id points into a stable registered feature ID.
    struct FeaturePath {
        std::string_view id;
        bool remote_supported{}, failed{}, notice_pending{};
        std::string reason;
    };

    // Routes one live native connection; raw may be a lobby ID while info contains the resolved Galaxy user ID.
    struct Connection {
        PeerId id;
        std::uint32_t native_slot{};
        std::uint64_t raw{};
        PeerInfo info;
        bool local{};
        Phase phase = Phase::AwaitHello;
        Clock::time_point deadline;
        // Identify this connection attempt so frames from earlier attempts can be rejected.
        std::uint64_t client_token{}, host_token{};
        std::vector<FeaturePath> paths;
        bool whole_notice{};
        // Preserve the first diagnostic for each rejected negotiation cause on this connection.
        std::uint8_t rejected{};
        std::string failure_reason;
        std::deque<OutgoingMessage> outgoing;
    };

    // Finds the live connection for this peer when the feature has the required client or host role.
    Connection *route(const FC_Feature &, PeerId) noexcept;
    const Connection *route(const FC_Feature &, PeerId) const noexcept;

    // Computes agreement for this feature and peer; the caller must hold mutex_.
    Agreement agreement_locked(const FC_Feature &, PeerId) const;

    // Finds the registration for this exact ID and a role frozen at Game Start.
    FC_Feature *implementation(std::string_view id, std::uint32_t role) const noexcept;

    // Allocates a fresh process-wide endpoint identity; zero is reserved for absence.
    static PeerId next_peer() noexcept;

    // Discards this connection's queued messages and releases their byte charges; the caller holds mutex_.
    void discard_messages(Connection &) noexcept;

    // Expires a connection and removes its messages from both queues, releasing their byte charges.
    void expire(Connection &);

    // Discards expired Hello frames and releases their incoming byte charges; the caller holds mutex_.
    // Pass Clock::time_point::max() to clear all retained frames at session, reset or transport failure boundaries.
    void expire_hellos(Clock::time_point now);

    // Accepts a validated Hello for this connection, checking its stage and token; the caller holds mutex_.
    void accept_hello(Connection &, std::uint64_t client_token, std::span<const std::string_view> manifest);

    // Fails this connection for every feature and discards messages whose reliable delivery cannot be preserved.
    void fail_connection(Connection &, std::string_view reason) noexcept;

    // Writes shared diagnostics through an available runtime, without patch attribution; callers hold mutex_.
    void log(FC_LogLevel, std::string_view text) const;

    // Describes the next negotiation operation for start, rejection and timeout diagnostics.
    static std::string_view pending_step(Phase) noexcept;

    // Expires a pending negotiation once at its existing three-second deadline; the caller holds mutex_.
    void timeout(Connection &, Clock::time_point now);

    // Reports the first occurrence of a negotiation rejection cause; the caller holds mutex_.
    void reject(Connection &, Rejection cause, std::string_view reason);

    // Generates a fresh nonzero attempt token using the process's operating-system random source.
    static std::optional<std::uint64_t> token() noexcept;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<FC_Feature>> features_;
    std::vector<Connection> connections_;
    PeerId host_, local_;
    PeerInfo host_info_, local_info_;
    GameMode mode_{};
    // Views into frozen Installed registrations; installed owners cannot unload.
    // Session boundaries clear the views before the frozen roles change.
    std::vector<std::string_view> client_manifest_, host_manifest_, feature_ids_;
    bool selected_{}, remote_available_{}, pumping_{};
    bool unknown_sender_logged_{};
    std::deque<IncomingMessage> incoming_;
    std::deque<PendingHello> pending_hellos_;
    std::size_t outgoing_bytes_{}, incoming_bytes_{};
};

// Returns the Multiplayer ABI table, which stays valid for the process lifetime and takes the calling patch as owner.
const FC_MultiplayerApi *multiplayer_api_host() noexcept;

} // namespace fc::runtime
