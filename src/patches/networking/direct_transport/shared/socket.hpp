#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string_view>

namespace fusioncutter::patches::direct_transport {

struct Endpoint {
    std::uint32_t ipv4_network_order{};
    std::uint16_t port_host_order{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return ipv4_network_order != 0 && port_host_order != 0;
    }

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

// Binds an uncommitted route to its first admissible source, then requires an exact endpoint match.
[[nodiscard]] inline constexpr bool endpoint_admits_source(const Endpoint& expected, const Endpoint& source,
                                                           bool allow_initial_bind) noexcept {
    return expected.valid() ? source == expected : allow_initial_bind;
}

// Rejects private, reserved, documentation, multicast, and other non-public IPv4 ranges.
[[nodiscard]] bool is_public_ipv4(std::uint32_t network_order) noexcept;
[[nodiscard]] std::string_view format_endpoint(const Endpoint& endpoint, std::span<char> output) noexcept;

struct SocketResult {
    std::int32_t value{-1};
    std::int32_t error{};

    [[nodiscard]] bool succeeded() const noexcept {
        return value >= 0;
    }
};

inline constexpr std::int32_t kRequestedSendBuffer = 64 * 1024;
inline constexpr std::int32_t kRequestedReceiveBuffer = 32 * 1024;
inline constexpr std::uint32_t kReceiveAdmissionLimit = 80;
inline constexpr std::int32_t kInvalidSocketOperation = 10022;
inline constexpr std::int32_t kSocketWouldBlock = 10035;
inline constexpr std::int32_t kSocketMessageTooLarge = 10040;

struct DatagramDrainResult {
    std::uint32_t discarded{};
    std::int32_t error{};
};

// Admits a bounded batch for processing while still draining excess datagrams from the socket.
template <typename Receiver, typename Handler>
[[nodiscard]] DatagramDrainResult drain_datagrams(Receiver&& receive, Handler&& handle) noexcept {
    DatagramDrainResult result;
    std::uint32_t received_datagrams{};
    for (;;) {
        Endpoint source{};
        const auto received = std::invoke(receive, source);
        if (!received.succeeded()) {
            if (received.error == kSocketWouldBlock) {
                return result;
            }
            if (received.error != kSocketMessageTooLarge) {
                result.error = received.error;
                return result;
            }
        }

        if (received_datagrams++ >= kReceiveAdmissionLimit) {
            ++result.discarded;
        } else if (received.succeeded()) {
            std::invoke(handle, received.value, source);
        }
    }
}

// Owns the nonblocking IPv4 UDP socket and its Winsock lifetime.
class UdpSocketRuntime {
  public:
    UdpSocketRuntime() = default;
    UdpSocketRuntime(const UdpSocketRuntime&) = delete;
    UdpSocketRuntime& operator=(const UdpSocketRuntime&) = delete;
    ~UdpSocketRuntime();

    // Opens, configures, and binds the socket; port zero requests an ephemeral client port.
    [[nodiscard]] bool start(std::uint16_t port, std::int32_t& error) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] SocketResult send(std::span<const std::uint8_t> bytes, const Endpoint& endpoint) noexcept;
    [[nodiscard]] SocketResult receive(std::span<std::uint8_t> bytes, Endpoint& endpoint) noexcept;

  private:
    using Handle = std::uintptr_t;
    static constexpr Handle kInvalidHandle = (std::numeric_limits<Handle>::max)();

    [[nodiscard]] bool start_win_sock(std::int32_t& error) noexcept;
    [[nodiscard]] bool open_socket(std::int32_t& error) noexcept;
    [[nodiscard]] bool configure_socket(std::int32_t& error) noexcept;
    [[nodiscard]] bool bind_socket(std::uint16_t port, std::int32_t& error) noexcept;

    Handle handle_{kInvalidHandle};
    std::uint16_t bound_port_{};
    bool win_sock_started_{};
};

} // namespace fusioncutter::patches::direct_transport
