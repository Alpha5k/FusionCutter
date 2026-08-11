#include "service.hpp"
#include "protocol.hpp"

#include <FusionCutter/reporting.hpp>

#include <WinSock2.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace fusioncutter::patches::rcon_server::gog {
namespace {

constexpr PatchId kPatchId = "RconServer";
constexpr std::size_t kMaximumClients = 8;
constexpr int kPollTimeoutMilliseconds = 50;
constexpr std::string_view kEndgame = "Game has ended";

[[nodiscard]] OutcomeReason service_error(std::string message, std::string operation) {
    return {std::move(message), std::move(operation), {}};
}

} // namespace

Service::Service(Game& game) noexcept : game_(game) {}

Service::~Service() {
    disable();
    clients_.clear();
    if (listener_ != INVALID_SOCKET) {
        closesocket(listener_);
    }
    if (winsock_started_) {
        WSACleanup();
    }
}

std::expected<void, OutcomeReason> Service::prepare() {
    try {
        return prepare_impl();
    } catch (const std::exception& error) {
        return std::unexpected(
            service_error("RCON preparation failed: " + std::string(error.what()), "Prepare RCON service"));
    }
}

void Service::enable() noexcept {
    enabled_.store(true, std::memory_order_release);
    start_condition_.notify_all();
}

void Service::disable() noexcept {
    worker_.request_stop();
    start_condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::expected<void, OutcomeReason> Service::prepare_impl() {
    if (auto validation = game_.validate(); !validation) {
        return validation;
    }

    WSADATA data{};
    const auto startup_result = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup_result != 0) {
        return std::unexpected(
            service_error("WSAStartup failed with error " + std::to_string(startup_result), "Start Windows sockets"));
    }
    winsock_started_ = true;

    if (auto authentication = authentication_.prepare(); !authentication) {
        return authentication;
    }

    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) {
        return std::unexpected(service_error(
            "RCON socket creation failed with error " + std::to_string(WSAGetLastError()), "Create RCON listener"));
    }

    u_long nonblocking = 1;
    if (ioctlsocket(listener_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        return std::unexpected(service_error("RCON socket setup failed with error " + std::to_string(WSAGetLastError()),
                                             "Configure RCON listener"));
    }

    const auto game_port = game_.game_port();
    if (game_port == 0) {
        return std::unexpected(service_error("GamePort is zero", "Bind RCON listener"));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(game_port);
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        return std::unexpected(service_error("RCON bind failed on GamePort " + std::to_string(game_port) +
                                                 " with error " + std::to_string(WSAGetLastError()),
                                             "Bind RCON listener"));
    }
    if (listen(listener_, static_cast<int>(kMaximumClients)) == SOCKET_ERROR) {
        return std::unexpected(
            service_error("RCON listen failed with error " + std::to_string(WSAGetLastError()), "Start RCON listener"));
    }

    clients_.reserve(kMaximumClients);
    // Runtime preparation may create the worker, but it remains gated until enable() runs after patch commit.
    worker_ = std::jthread([this](std::stop_token stop_token) noexcept {
        try {
            worker_main(stop_token);
        } catch (const std::exception& error) {
            logging::error(kPatchId, "RCON worker stopped unexpectedly: " + std::string(error.what()),
                           "Run RCON service");
        } catch (...) {
            logging::error(kPatchId, "RCON worker stopped unexpectedly", "Run RCON service");
        }
    });
    return {};
}

void Service::worker_main(std::stop_token stop_token) {
    std::mutex start_mutex;
    std::unique_lock start_lock(start_mutex);
    if (!start_condition_.wait(start_lock, stop_token, [this] {
            return enabled_.load(std::memory_order_acquire);
        })) {
        return;
    }
    start_lock.unlock();

    previous_map_idle_ = game_.map_is_idle();
    logging::info(kPatchId, "RCON service started");
    while (!stop_token.stop_requested()) {
        if (!poll_sockets()) {
            break;
        }
        pump_notifications();
        std::erase_if(clients_, [](const Client& client) {
            return !client.valid();
        });
    }
    clients_.clear();
}

bool Service::poll_sockets() {
    std::array<WSAPOLLFD, kMaximumClients + 1> descriptors{};
    descriptors[0] = {.fd = listener_, .events = POLLRDNORM};
    for (std::size_t index = 0; index < clients_.size(); ++index) {
        descriptors[index + 1] = {.fd = clients_[index].socket(), .events = clients_[index].poll_events()};
    }

    const auto count = static_cast<ULONG>(clients_.size() + 1);
    const auto result = WSAPoll(descriptors.data(), count, kPollTimeoutMilliseconds);
    if (result == SOCKET_ERROR) {
        logging::error(kPatchId, "RCON socket polling failed with error " + std::to_string(WSAGetLastError()),
                       "Poll RCON sockets");
        return false;
    }
    if (result == 0) {
        return true;
    }

    if ((descriptors[0].revents & POLLRDNORM) != 0) {
        accept_clients();
    }
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        logging::error(kPatchId, "RCON listener reported a socket failure", "Poll RCON listener");
        return false;
    }

    for (std::size_t index = 0; index < clients_.size(); ++index) {
        clients_[index].handle_events(descriptors[index + 1].revents, game_, authentication_);
    }
    return true;
}

void Service::accept_clients() {
    for (std::size_t attempt = 0; attempt < kMaximumClients; ++attempt) {
        const auto accepted = accept(listener_, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                logging::warning(kPatchId, "RCON client accept failed with error " + std::to_string(WSAGetLastError()),
                                 "Accept RCON client");
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
    const auto current_map_idle = game_.map_is_idle();
    if (previous_map_idle_ && !current_map_idle) {
        endgame_pending_ = true;
    }
    previous_map_idle_ = current_map_idle;

    // The game can announce map end before its final chat callback is drained. Delay the end marker until all queued
    // chat is broadcast so RCON clients observe the established ordering.
    std::array<Game::ChatMessage, Game::kChatPumpLimit> pending{};
    const auto result = game_.drain_chat(pending);
    for (std::size_t index = 0; index < result.count; ++index) {
        broadcast({pending[index].text.data(), pending[index].size});
    }
    if (result.empty && endgame_pending_) {
        broadcast(kEndgame);
        endgame_pending_ = false;
    }
}

void Service::broadcast(std::string_view message) {
    const auto packet = protocol::encode_message(message);
    for (auto& client : clients_) {
        if (client.valid() && client.authenticated()) {
            client.queue_packet(packet);
        }
    }
}

} // namespace fusioncutter::patches::rcon_server::gog
