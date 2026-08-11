#pragma once

#include "protocol.hpp"

#include <FusionCutter/reporting.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace fusioncutter::patches::direct_transport {

enum class RouteReason : std::uint8_t {
    None,
    CapabilityTimeout,
    ProofTimeout,
    ReadyTimeout,
    CommitTimeout,
    ProtocolIncompatible,
    SocketUnavailable,
    EndpointUnavailable,
    Commit,
    ImplicitCommit,
    RandomGenerationFailed,
};

enum class AssociationEndReason : std::uint8_t {
    Disconnected,
    Replaced,
    HostChanged,
    Reset,
    LobbyLeft,
    ControlLimit,
    PolicyRemoval,
    Shutdown,
};

enum class RejectionKind : std::uint8_t {
    Endpoint,
    Authentication,
    Replay,
    Invalid,
};

[[nodiscard]] const char* route_reason_name(RouteReason reason) noexcept;
// Limits each association to forward progress toward one immutable terminal route.
[[nodiscard]] constexpr bool valid_route_transition(RouteState from, RouteState to) noexcept {
    if (from == to) {
        return true;
    }
    switch (from) {
    case RouteState::Unclassified:
        return to == RouteState::Negotiating || to == RouteState::GalaxyLocked;
    case RouteState::Negotiating:
        return to == RouteState::AwaitCommit || to == RouteState::GalaxyLocked;
    case RouteState::AwaitCommit:
        return to == RouteState::DirectLocked || to == RouteState::GalaxyLocked;
    case RouteState::GalaxyLocked:
    case RouteState::DirectLocked:
        return false;
    }
    return false;
}

// Rate-limits recurring runtime failures without allocating or retaining peer-controlled input.
class RuntimeLog {
  public:
    // Rate-limits recurring failures by operation and severity.
    void error(std::string_view message, std::string_view operation) noexcept;
    void warning(std::string_view message, std::string_view operation) noexcept;
    // Logs ordinary network failures at Debug and local socket misuse at Error.
    void socket(std::string_view operation, std::int32_t error, bool internal_failure = false) noexcept;
    // Reports cumulative datagrams discarded after the bounded receive budget.
    void receive_overflow(std::uint32_t discarded) noexcept;

  private:
    struct Entry {
        std::string_view operation;
        std::int32_t error{};
        LogLevel level{LogLevel::Off};
        std::uint64_t occurrences{};
    };

    [[nodiscard]] std::uint64_t record(LogLevel level, std::string_view operation, std::int32_t error) noexcept;

    std::array<Entry, 16> entries_{};
    std::uint64_t discarded_datagrams_{};
    std::uint64_t next_overflow_report_{1};
};

// Keeps only the bounded counters needed for one association's terminal and end diagnostics.
class AssociationDiagnostics {
  public:
    void reset(std::uint64_t now) noexcept;
    // Records the first terminal carrier decision for the association.
    void terminal_route(std::string_view role, std::uint8_t slot, std::uint32_t generation, std::uint32_t connection_id,
                        RouteState route, RouteReason reason, std::uint64_t now, std::uint32_t attempts = 0) noexcept;
    // Records entry into direct traffic and its bounded packet counters.
    void direct_tx(std::string_view role, std::uint8_t slot, std::uint32_t generation,
                   std::uint32_t connection_id) noexcept;
    void direct_rx(std::string_view role, std::uint8_t slot, std::uint32_t generation,
                   std::uint32_t connection_id) noexcept;
    // Accumulates silent rejection, send-failure, and policy-action counters for the final summary.
    void reject(RejectionKind kind) noexcept;
    void send_failed() noexcept;
    void mark_policy_action() noexcept;
    // Emits the end-of-association summary when it contains useful diagnostics.
    void finish(std::string_view role, std::uint8_t slot, std::uint32_t generation, std::uint32_t connection_id,
                RouteState route, AssociationEndReason reason, std::uint64_t now) noexcept;

    [[nodiscard]] RouteReason route_reason() const noexcept;

  private:
    std::uint64_t started_ms_{};
    std::uint64_t direct_started_ms_{};
    std::uint32_t tx_datagrams_{};
    std::uint32_t rx_datagrams_{};
    std::uint32_t send_failures_{};
    std::uint32_t endpoint_rejects_{};
    std::uint32_t authentication_rejects_{};
    std::uint32_t replay_rejects_{};
    std::uint32_t invalid_rejects_{};
    RouteReason route_reason_{RouteReason::None};
    bool terminal_reported_{};
    bool direct_entered_{};
    bool first_tx_reported_{};
    bool first_rx_reported_{};
    bool policy_action_{};
};

// Separates local socket misuse from expected runtime network failures.
[[nodiscard]] bool is_internal_socket_error(std::int32_t error) noexcept;

} // namespace fusioncutter::patches::direct_transport
