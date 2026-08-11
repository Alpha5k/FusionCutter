#pragma once

#include <WinSock2.h>

#include <array>
#include <cstddef>
#include <deque>
#include <vector>

namespace fusioncutter::patches::rcon_server::gog {

class Authentication;
class Game;

// Owns one nonblocking RCON connection and its authentication, framing, and output state.
class Client {
  public:
    explicit Client(SOCKET socket) noexcept;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;

    [[nodiscard]] SOCKET socket() const noexcept;
    [[nodiscard]] short poll_events() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool authenticated() const noexcept;

    // Dispatches one socket-poll result without blocking the service worker.
    void handle_events(short events, Game& game, Authentication& authentication);

    void queue_packet(std::vector<char> packet);
    void disconnect() noexcept;

  private:
    static constexpr std::size_t kInputCapacity = 0x400;
    static constexpr std::size_t kMaximumQueuedBytes = 64 * 1024;

    void receive(Game& game, Authentication& authentication);

    // Advances login and command frames until the client needs more input.
    void process_input(Game& game, Authentication& authentication);

    void consume_input(std::size_t size) noexcept;

    // Flushes queued packets until the socket would block or the queue is empty.
    void send_pending() noexcept;
    void move_from(Client&& other) noexcept;

    SOCKET socket_{INVALID_SOCKET};
    bool authenticated_{};
    bool close_after_send_{};
    std::array<char, kInputCapacity> input_{};
    std::size_t input_size_{};
    std::deque<std::vector<char>> output_;
    std::size_t output_offset_{};
    std::size_t queued_bytes_{};
};

} // namespace fusioncutter::patches::rcon_server::gog
