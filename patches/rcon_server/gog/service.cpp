#include "service.hpp"

#include "protocol.hpp"
#include <fc/reporting.hpp>

#include <WinSock2.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <exception>
#include <format>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace fc::core_plugin::rcon_server::gog {
namespace {

constexpr std::size_t kMaximumAcceptsPerPoll = 8;
constexpr int kPollTimeoutMilliseconds = 50;
constexpr std::string_view kEndgame = "Game has ended";

// Adds the failed service operation to its error message.
fc::Result<> service_error(std::string message, std::string operation) {
    return std::unexpected(fc::Error{operation + ": " + message});
}

// Formats a Winsock error code with Windows' corresponding description.
std::string winsock_error(int code) {
    return std::format("Winsock error {}: {}", code, std::error_code{code, std::system_category()}.message());
}

} // namespace

Service::Service(GameBridge &game, fc::Logger logger) noexcept : game_(game), logger_(logger) {}

Service::~Service() {
    // Stop and join the worker before releasing the sockets and Winsock reference it uses.
    worker_.request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }

    clients_.clear();
    if (listener_ != INVALID_SOCKET) {
        closesocket(listener_);
    }

    if (winsock_started_) {
        WSACleanup();
    }
}

void Service::activate() noexcept {
    // Publish the initial winner state before waking the worker; the mutex prevents a missed activation wakeup.
    had_winner_ = game_.has_winner();
    {
        std::lock_guard lock{start_mutex_};
        active_.store(true, std::memory_order_release);
    }
    start_condition_.notify_all();
}

void Service::write_status(fc::StatusContext &status) const noexcept {
    status.line("Active: {}; TCP port: {}; clients: {}", active_.load(std::memory_order_acquire), port_,
                current_clients_.load(std::memory_order_relaxed));
}

fc::Result<> Service::prepare() {
    // Winsock is process-wide and reference counted; this instance releases exactly one successful startup.
    WSADATA data{};
    const auto startup_result = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup_result != 0) {
        return service_error(winsock_error(startup_result), "Could not start Winsock for RCON");
    }
    winsock_started_ = true;

    if (auto authentication = authentication_.prepare(); !authentication) {
        return authentication;
    }

    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) {
        return service_error(winsock_error(WSAGetLastError()), "Could not create the RCON listener socket");
    }

    u_long nonblocking = 1;
    if (ioctlsocket(listener_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        return service_error(winsock_error(WSAGetLastError()), "Could not make the RCON listener socket nonblocking");
    }

    port_ = game_.direct_port();
    if (port_ == 0) {
        return service_error("the game server port is 0", "Could not bind the RCON listener");
    }

    // Legacy administration tools connect to TCP on the configured game port, while the game itself uses UDP.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port_);
    if (bind(listener_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
        return service_error(winsock_error(WSAGetLastError()) + " on game port " + std::to_string(port_),
                             "Could not bind the RCON listener");
    }

    if (listen(listener_, static_cast<int>(kMaximumClients)) == SOCKET_ERROR) {
        return service_error(winsock_error(WSAGetLastError()), "Could not listen for RCON connections");
    }

    clients_.reserve(kMaximumClients);

    // Preparation starts a dormant worker; activation releases its client and native command work.
    worker_ = std::jthread([this](std::stop_token stop_token) noexcept {
        try {
            worker_main(stop_token);
        } catch (const std::exception &error) {
            logger_.error("RCON service stopped after an unexpected exception: {}", error.what());
            fc::fatal(fc::Error{"RCON worker failed unexpectedly"});
        } catch (...) {
            logger_.error("RCON service stopped after an unexpected exception");
            fc::fatal(fc::Error{"RCON worker failed unexpectedly"});
        }
    });
    logger_.debug("RCON listener prepared on TCP port {}", port_);
    return {};
}

void Service::worker_main(std::stop_token stop_token) {
    // Wait for native installation before servicing clients; stopping the service also cancels this wait.
    std::unique_lock start_lock(start_mutex_);
    if (!start_condition_.wait(start_lock, stop_token, [this] {
            return active_.load(std::memory_order_acquire);
        })) {
        return;
    }
    start_lock.unlock();

    // Handle bounded socket work, then forward notifications before removing disconnected clients.
    while (!stop_token.stop_requested()) {
        if (!poll_sockets()) {
            fc::fatal(fc::Error{"Required RCON listener or socket polling failed"});
        }

        pump_notifications();
        std::erase_if(clients_, [](const Client &client) {
            return !client.valid();
        });
        current_clients_.store(static_cast<std::uint32_t>(clients_.size()), std::memory_order_relaxed);
    }
}

bool Service::poll_sockets() {
    // Keep descriptor indices tied to the original clients; newly accepted clients wait until the next poll.
    std::array<WSAPOLLFD, kMaximumClients + 1> descriptors{};
    descriptors[0] = {.fd = listener_, .events = POLLRDNORM};
    const auto polled_clients = clients_.size();
    for (std::size_t index = 0; index < polled_clients; ++index) {
        descriptors[index + 1] = {.fd = clients_[index].socket(), .events = clients_[index].poll_events()};
    }

    const auto count = static_cast<ULONG>(polled_clients + 1);
    const auto result = WSAPoll(descriptors.data(), count, kPollTimeoutMilliseconds);
    if (result == SOCKET_ERROR) {
        const auto error = WSAGetLastError();
        logger_.error("RCON socket poll failed; the service stopped ({})", winsock_error(error));
        return false;
    }

    if ((descriptors[0].revents & POLLRDNORM) != 0) {
        accept_clients();
    }
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        logger_.error("RCON listener reported socket events 0x{:X}; the service stopped",
                      static_cast<unsigned int>(descriptors[0].revents));
        return false;
    }

    for (std::size_t index = 0; index < polled_clients; ++index) {
        if (clients_[index].handle_events(descriptors[index + 1].revents, game_, authentication_)) {
            report_output_limit();
        }
    }
    return true;
}

void Service::accept_clients() {
    // Limit accepts per cycle so incoming connections cannot starve existing clients or game notifications.
    for (std::size_t attempt = 0; attempt < kMaximumAcceptsPerPoll; ++attempt) {
        const auto accepted = accept(listener_, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            const auto error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && !logged_accept_failure_) {
                logged_accept_failure_ = true;
                logger_.warning("Could not accept an RCON connection; the listener remains active ({})", winsock_error(error));
            }
            return;
        }

        if (clients_.size() == kMaximumClients) {
            closesocket(accepted);
            continue;
        }

        u_long nonblocking = 1;
        if (ioctlsocket(accepted, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            closesocket(accepted);
            continue;
        }

        clients_.emplace_back(accepted);
    }
}

void Service::pump_notifications() {
    // Keep a winning-team transition pending across bounded chat drains.
    const auto has_winner = game_.has_winner();
    if (!had_winner_ && has_winner) {
        endgame_pending_ = true;
    }
    had_winner_ = has_winner;

    // Broadcast captured chat before the pending endgame marker.
    // Keep the marker pending until a drain reports the queue empty.
    std::array<GameBridge::ChatMessage, GameBridge::kChatPumpLimit> pending{};
    const auto result = game_.drain_chat(pending);
    for (std::size_t index = 0; index < result.count; ++index) {
        broadcast({pending[index].text.data(), pending[index].size});
    }

    if (result.queue_empty && endgame_pending_) {
        broadcast(kEndgame);
        endgame_pending_ = false;
    }

    if (!logged_chat_drop_ && game_.dropped_chat_messages() != 0) {
        logged_chat_drop_ = true;
        logger_.warning("Some chat messages were dropped from RCON output");
    }
}

void Service::broadcast(std::string_view message) {
    const auto has_recipient = std::ranges::any_of(clients_, [](const Client &client) {
        return client.valid() && client.authenticated();
    });
    if (!has_recipient) {
        return;
    }

    // Encode once, then give each authenticated client its own queued packet.
    const auto packet = protocol::encode_message(message);
    for (auto &client : clients_) {
        if (!client.valid() || !client.authenticated()) {
            continue;
        }
        if (!client.queue_packet(packet)) {
            report_output_limit();
        }
    }
}

void Service::report_output_limit() noexcept {
    if (!logged_output_limit_) {
        logged_output_limit_ = true;
        logger_.warning("Disconnected an RCON client because its pending output exceeded the queue limit");
    }
}

} // namespace fc::core_plugin::rcon_server::gog
