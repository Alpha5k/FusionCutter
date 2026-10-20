#pragma once

#include <WinSock2.h>

#include <array>
#include <cstddef>
#include <vector>

namespace fc::core_plugin::rcon_server::gog {

class Authentication;
class GameBridge;

// Owns one nonblocking RCON connection and its authentication, framing, and bounded output state.
// The service worker serializes all access to a client's state.
class Client {
  public:
    // Takes ownership of a socket that the caller has already configured for nonblocking I/O.
    explicit Client(SOCKET socket) noexcept;

    // Closes the owned socket and discards pending output.
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // Transfers the connection and buffered traffic, leaving other without socket ownership.
    Client(Client &&other) noexcept;
    // Closes this connection before taking other's connection and buffered traffic.
    Client &operator=(Client &&other) noexcept;

    SOCKET socket() const noexcept;
    // Requests reads until login rejection and writes while output remains queued.
    short poll_events() const noexcept;
    bool valid() const noexcept;
    bool authenticated() const noexcept;

    // Advances buffered protocol work for one poll; game and authentication must be prepared before input is processed.
    // Returns true only when bounded output storage forced the client to disconnect.
    bool handle_events(short events, GameBridge &game, Authentication &authentication);

    // Takes one encoded packet for ordered delivery; returns false and disconnects if the unsent byte limit is exceeded.
    bool queue_packet(std::vector<char> packet);

    // Closes the socket and discards pending output; repeated calls are harmless.
    void disconnect() noexcept;

  private:
    static constexpr std::size_t kInputCapacity = 0x400;
    static constexpr std::size_t kMaximumQueuedBytes = 64 * 1024;
    static constexpr std::size_t kMaximumFramesPerPoll = 8;

    // Appends one socket read to buffered input; disconnects on a full buffer, peer close or socket failure.
    void receive() noexcept;

    // Shares processed_frames across this poll's input passes and stops at the frame limit.
    // Returns true only if queuing a response exceeds the output byte limit.
    bool process_input(GameBridge &game, Authentication &authentication, std::size_t &processed_frames);

    // Removes size consumed bytes, retaining incomplete or subsequent frames; size must not exceed input_size_.
    void consume_input(std::size_t size) noexcept;

    // Sends the unsent byte span once and preserves any remainder for a later poll cycle.
    void send_pending() noexcept;

    // Transfers the native socket and every partial input or output position without duplicating ownership.
    void move_from(Client &&other) noexcept;

    SOCKET socket_{INVALID_SOCKET};
    bool authenticated_{};
    // After a rejected login, send its one-byte response and close the connection.
    bool close_after_send_{};
    std::array<char, kInputCapacity> input_{};
    std::size_t input_size_{};

    // Encoded responses and notifications in delivery order; the limit counts only the unsent suffix.
    std::vector<char> output_;
    // Bytes already handed to the socket.
    std::size_t output_offset_{};
};

} // namespace fc::core_plugin::rcon_server::gog
