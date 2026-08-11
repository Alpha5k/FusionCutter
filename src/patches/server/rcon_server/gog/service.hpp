#pragma once

#include "client.hpp"
#include "authentication.hpp"
#include "game.hpp"

#include <FusionCutter/outcome.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <string_view>
#include <thread>
#include <vector>

namespace fusioncutter::patches::rcon_server::gog {

// Runs the RCON listener, clients, and game notifications on one nonblocking worker.
class Service {
  public:
    explicit Service(Game& game) noexcept;
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    // Prepares authentication, the listener, and a worker gated until patch activation.
    [[nodiscard]] std::expected<void, OutcomeReason> prepare();

    void enable() noexcept;
    void disable() noexcept;

  private:
    [[nodiscard]] std::expected<void, OutcomeReason> prepare_impl();

    // Polls clients and forwards game notifications until shutdown.
    void worker_main(std::stop_token stop_token);

    // Processes one bounded poll cycle for the listener and active clients.
    [[nodiscard]] bool poll_sockets();

    // Accepts a bounded batch of pending clients and makes each socket nonblocking.
    void accept_clients();

    // Broadcasts captured chat and the ordered endgame notification.
    void pump_notifications();

    // Queues one encoded notification for every authenticated client.
    void broadcast(std::string_view message);

    Game& game_;
    Authentication authentication_;
    SOCKET listener_{INVALID_SOCKET};
    bool winsock_started_{};
    std::vector<Client> clients_;
    std::jthread worker_;
    std::atomic_bool enabled_{};
    std::condition_variable_any start_condition_;
    bool previous_map_idle_{};
    bool endgame_pending_{};
};

} // namespace fusioncutter::patches::rcon_server::gog
