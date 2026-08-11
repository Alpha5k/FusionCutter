#include "diagnostics.hpp"

#include "socket.hpp"

#include <WinSock2.h>

#include <bit>
#include <cstdio>
#include <limits>

namespace fusioncutter::patches::direct_transport {
namespace {

constexpr PatchId kPatchId = "DirectTransport";

[[nodiscard]] const char* route_name(RouteState route) noexcept {
    return route == RouteState::DirectLocked ? "Direct" : "Galaxy";
}

[[nodiscard]] const char* end_reason_name(AssociationEndReason reason) noexcept {
    switch (reason) {
    case AssociationEndReason::Disconnected:
        return "Disconnected";
    case AssociationEndReason::Replaced:
        return "Replaced";
    case AssociationEndReason::HostChanged:
        return "HostChanged";
    case AssociationEndReason::Reset:
        return "Reset";
    case AssociationEndReason::LobbyLeft:
        return "LobbyLeft";
    case AssociationEndReason::ControlLimit:
        return "ControlLimit";
    case AssociationEndReason::PolicyRemoval:
        return "PolicyRemoval";
    case AssociationEndReason::Shutdown:
        return "Shutdown";
    }
    return "Unknown";
}

[[nodiscard]] std::uint64_t elapsed(std::uint64_t now, std::uint64_t start) noexcept {
    return now >= start ? now - start : 0;
}

} // namespace

const char* route_reason_name(RouteReason reason) noexcept {
    switch (reason) {
    case RouteReason::None:
        return "Pending";
    case RouteReason::CapabilityTimeout:
        return "Capability timeout";
    case RouteReason::ProofTimeout:
        return "Proof timeout";
    case RouteReason::ReadyTimeout:
        return "Ready timeout";
    case RouteReason::CommitTimeout:
        return "Commit timeout";
    case RouteReason::ProtocolIncompatible:
        return "Protocol incompatible";
    case RouteReason::SocketUnavailable:
        return "Socket unavailable";
    case RouteReason::EndpointUnavailable:
        return "Endpoint unavailable";
    case RouteReason::Commit:
        return "Commit";
    case RouteReason::ImplicitCommit:
        return "Implicit commit";
    case RouteReason::RandomGenerationFailed:
        return "Random generation failed";
    }
    return "Unknown";
}

std::uint64_t RuntimeLog::record(LogLevel level, std::string_view operation, std::int32_t error) noexcept {
    auto* available = static_cast<Entry*>(nullptr);
    for (auto& entry : entries_) {
        if (entry.level == level && entry.error == error && entry.operation == operation) {
            return ++entry.occurrences;
        }
        if (entry.level == LogLevel::Off && available == nullptr) {
            available = &entry;
        }
    }
    if (available == nullptr) {
        return 0;
    }
    *available = {operation, error, level, 1};
    return 1;
}

void RuntimeLog::error(std::string_view message, std::string_view operation) noexcept {
    const auto occurrence = record(LogLevel::Error, operation, 0);
    if (occurrence == 0 || !std::has_single_bit(occurrence) || !logging::enabled(LogLevel::Error)) {
        return;
    }
    char text[256]{};
    std::snprintf(text, sizeof(text), "%.*s (occurrence %llu)", static_cast<int>(message.size()), message.data(),
                  static_cast<unsigned long long>(occurrence));
    logging::error(kPatchId, text, operation);
}

void RuntimeLog::warning(std::string_view message, std::string_view operation) noexcept {
    const auto occurrence = record(LogLevel::Warning, operation, 0);
    if (occurrence == 0 || !std::has_single_bit(occurrence) || !logging::enabled(LogLevel::Warning)) {
        return;
    }
    char text[256]{};
    std::snprintf(text, sizeof(text), "%.*s (occurrence %llu)", static_cast<int>(message.size()), message.data(),
                  static_cast<unsigned long long>(occurrence));
    logging::warning(kPatchId, text, operation);
}

void RuntimeLog::socket(std::string_view operation, std::int32_t error, bool internal_failure) noexcept {
    const auto level = internal_failure ? LogLevel::Error : LogLevel::Debug;
    const auto occurrence = record(level, operation, error);
    if (occurrence == 0 || !std::has_single_bit(occurrence) || !logging::enabled(level)) {
        return;
    }
    char text[128]{};
    std::snprintf(text, sizeof(text), "Windows Sockets error %d (occurrence %llu)", error,
                  static_cast<unsigned long long>(occurrence));
    logging::write(level, kPatchId, text, operation);
}

void RuntimeLog::receive_overflow(std::uint32_t discarded) noexcept {
    if (discarded == 0) {
        return;
    }
    discarded_datagrams_ += discarded;
    if (discarded_datagrams_ < next_overflow_report_) {
        return;
    }
    while (next_overflow_report_ <= discarded_datagrams_ &&
           next_overflow_report_ <= (std::numeric_limits<std::uint64_t>::max)() / 2) {
        next_overflow_report_ *= 2;
    }
    if (!logging::enabled(LogLevel::Warning)) {
        return;
    }
    char text[160]{};
    std::snprintf(text, sizeof(text), "Receive budget overflow discarded %llu direct UDP datagrams",
                  static_cast<unsigned long long>(discarded_datagrams_));
    logging::warning(kPatchId, text, "Drain direct UDP datagrams");
}

void AssociationDiagnostics::reset(std::uint64_t now) noexcept {
    *this = {};
    started_ms_ = now;
}

void AssociationDiagnostics::terminal_route(std::string_view role, std::uint8_t slot, std::uint32_t generation,
                                            std::uint32_t connection_id, RouteState route, RouteReason reason,
                                            std::uint64_t now, std::uint32_t attempts) noexcept {
    if (reason != RouteReason::None) {
        route_reason_ = reason;
    }
    if (route == RouteState::DirectLocked && !direct_entered_) {
        direct_entered_ = true;
        direct_started_ms_ = now;
    }
    if (terminal_reported_ || !logging::enabled(LogLevel::Debug)) {
        terminal_reported_ = true;
        return;
    }
    terminal_reported_ = true;
    char text[256]{};
    std::snprintf(text, sizeof(text),
                  "role=%.*s slot=%u generation=%u connection=%u route=%s reason=%s elapsed_ms=%llu attempts=%u",
                  static_cast<int>(role.size()), role.data(), slot, generation, connection_id, route_name(route),
                  route_reason_name(route_reason_), static_cast<unsigned long long>(elapsed(now, started_ms_)),
                  attempts);
    logging::debug(kPatchId, text, "Select association route");
}

void AssociationDiagnostics::direct_tx(std::string_view role, std::uint8_t slot, std::uint32_t generation,
                                       std::uint32_t connection_id) noexcept {
    ++tx_datagrams_;
    if (first_tx_reported_ || !logging::enabled(LogLevel::Debug)) {
        first_tx_reported_ = true;
        return;
    }
    first_tx_reported_ = true;
    char text[160]{};
    std::snprintf(text, sizeof(text), "role=%.*s slot=%u generation=%u connection=%u first_direct_tx=1",
                  static_cast<int>(role.size()), role.data(), slot, generation, connection_id);
    logging::debug(kPatchId, text, "Transmit direct game data");
}

void AssociationDiagnostics::direct_rx(std::string_view role, std::uint8_t slot, std::uint32_t generation,
                                       std::uint32_t connection_id) noexcept {
    ++rx_datagrams_;
    if (first_rx_reported_ || !logging::enabled(LogLevel::Debug)) {
        first_rx_reported_ = true;
        return;
    }
    first_rx_reported_ = true;
    char text[160]{};
    std::snprintf(text, sizeof(text), "role=%.*s slot=%u generation=%u connection=%u first_direct_rx=1",
                  static_cast<int>(role.size()), role.data(), slot, generation, connection_id);
    logging::debug(kPatchId, text, "Deliver direct game data");
}

void AssociationDiagnostics::reject(RejectionKind kind) noexcept {
    switch (kind) {
    case RejectionKind::Endpoint:
        ++endpoint_rejects_;
        break;
    case RejectionKind::Authentication:
        ++authentication_rejects_;
        break;
    case RejectionKind::Replay:
        ++replay_rejects_;
        break;
    case RejectionKind::Invalid:
        ++invalid_rejects_;
        break;
    }
}

void AssociationDiagnostics::send_failed() noexcept {
    ++send_failures_;
}

void AssociationDiagnostics::mark_policy_action() noexcept {
    policy_action_ = true;
}

void AssociationDiagnostics::finish(std::string_view role, std::uint8_t slot, std::uint32_t generation,
                                    std::uint32_t connection_id, RouteState route, AssociationEndReason reason,
                                    std::uint64_t now) noexcept {
    if (!direct_entered_ && !policy_action_ && send_failures_ == 0 && endpoint_rejects_ == 0 &&
        authentication_rejects_ == 0 && replay_rejects_ == 0 && invalid_rejects_ == 0) {
        return;
    }
    if (!logging::enabled(LogLevel::Debug)) {
        return;
    }
    char text[384]{};
    const auto direct_duration = direct_entered_ ? elapsed(now, direct_started_ms_) : 0;
    std::snprintf(text, sizeof(text),
                  "role=%.*s slot=%u generation=%u connection=%u end=%s route=%s direct_ms=%llu tx=%u rx=%u "
                  "send_failures=%u endpoint_rejects=%u authentication_rejects=%u replay_rejects=%u invalid_rejects=%u",
                  static_cast<int>(role.size()), role.data(), slot, generation, connection_id, end_reason_name(reason),
                  route_name(route), static_cast<unsigned long long>(direct_duration), tx_datagrams_, rx_datagrams_,
                  send_failures_, endpoint_rejects_, authentication_rejects_, replay_rejects_, invalid_rejects_);
    logging::debug(kPatchId, text, "End association");
}

RouteReason AssociationDiagnostics::route_reason() const noexcept {
    return route_reason_;
}

bool is_internal_socket_error(std::int32_t error) noexcept {
    return error == kInvalidSocketOperation || error == WSAENOTSOCK || error == WSAESHUTDOWN ||
           error == WSANOTINITIALISED || error == WSAEFAULT || error == WSAEAFNOSUPPORT;
}

} // namespace fusioncutter::patches::direct_transport
