#pragma once

#include "authentication.hpp"
#include "client.hpp"
#include "game.hpp"

#include <fc/plugin.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <string_view>
#include <thread>
#include <mutex>
#include <vector>

namespace fc::core_plugin::rcon_server::gog {

// Owns the loopback listener; one worker serializes client authentication, commands and game notifications.
class Service {
  public:
    // Borrows the game bridge, which must outlive the service and its worker.
    Service(GameBridge &game, fc::Logger logger) noexcept;

    // Stops and joins the worker before releasing clients, the listener and this service's Winsock reference.
    ~Service();

    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;

    // Call once after game preparation to acquire authentication, a nonblocking listener and a waiting worker.
    // Destruction releases acquired resources even if preparation fails.
    fc::Result<> prepare();

    // Call once after successful preparation and native installation to start servicing clients and commands.
    void activate() noexcept;

    // Appends listener activity, port and client count to the runtime status report.
    void write_status(fc::StatusContext &status) const noexcept;

  private:
    static constexpr std::size_t kMaximumClients = 8;

    // Polls clients and forwards game notifications until shutdown or a listener failure.
    void worker_main(std::stop_token stop_token);

    // Polls one fixed socket set and returns false after logging a listener or polling failure.
    bool poll_sockets();

    // Accepts a fixed batch and rejects connections beyond the service's client capacity.
    void accept_clients();

    // Drains captured chat before emitting a pending endgame marker when that drain reports an empty queue.
    void pump_notifications();

    // Queues one encoded notification for each currently authenticated client.
    void broadcast(std::string_view message);

    // Warns once when a client's pending output exceeds the queue limit and causes disconnection.
    void report_output_limit() noexcept;

    // Handler declares its game bridge before the service so the bridge outlives worker shutdown.
    GameBridge &game_;
    fc::Logger logger_;
    Authentication authentication_;

    // Prepared on the control thread, then polled and accepted by the service worker.
    SOCKET listener_{INVALID_SOCKET};
    bool winsock_started_{};
    std::uint16_t port_{};
    // Owned by the worker after activation; current_clients_ supplies the count to status callbacks.
    std::vector<Client> clients_;
    std::jthread worker_;
    std::mutex start_mutex_;
    std::condition_variable_any start_condition_;
    bool had_winner_{};
    // Remembers an endgame marker until a chat drain reports an empty queue.
    bool endgame_pending_{};
    bool logged_output_limit_{};
    bool logged_accept_failure_{};
    bool logged_chat_drop_{};
    std::atomic_bool active_{};
    std::atomic_uint32_t current_clients_{};
};

} // namespace fc::core_plugin::rcon_server::gog
