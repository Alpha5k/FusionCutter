#include "socket.hpp"

#include <WinSock2.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace fusioncutter::patches::direct_transport {
namespace {

[[nodiscard]] SOCKET to_socket(std::uintptr_t handle) noexcept {
    return static_cast<SOCKET>(handle);
}

[[nodiscard]] bool set_buffer_option(SOCKET handle, int option, std::int32_t requested, std::int32_t& error) noexcept {
    if (setsockopt(handle, SOL_SOCKET, option, reinterpret_cast<const char*>(&requested), sizeof(requested)) ==
        SOCKET_ERROR) {
        error = WSAGetLastError();
        return false;
    }
    return true;
}

[[nodiscard]] consteval std::uint32_t pack_ipv4(std::array<std::uint8_t, 4> octets) {
    return (static_cast<std::uint32_t>(octets[0]) << 24) | (static_cast<std::uint32_t>(octets[1]) << 16) |
           (static_cast<std::uint32_t>(octets[2]) << 8) | octets[3];
}

[[nodiscard]] consteval std::uint32_t prefix_mask(std::uint8_t length) {
    return length == 0 ? 0 : ~std::uint32_t{} << (32 - length);
}

// Represents one IPv4 CIDR in host order for readable address admission rules.
class Ipv4Cidr {
  public:
    consteval Ipv4Cidr(std::array<std::uint8_t, 4> octets, std::uint8_t prefix_length)
        : mask_(prefix_mask(prefix_length)), network_(pack_ipv4(octets) & mask_) {}

    [[nodiscard]] constexpr bool contains(std::uint32_t address) const noexcept {
        return (address & mask_) == network_;
    }

  private:
    std::uint32_t mask_;
    std::uint32_t network_;
};

constexpr std::array kNonPublicIpv4Ranges{
    Ipv4Cidr{{0, 0, 0, 0}, 8},       // 0.0.0.0/8
    Ipv4Cidr{{10, 0, 0, 0}, 8},      // 10.0.0.0/8
    Ipv4Cidr{{100, 64, 0, 0}, 10},   // 100.64.0.0/10
    Ipv4Cidr{{127, 0, 0, 0}, 8},     // 127.0.0.0/8
    Ipv4Cidr{{169, 254, 0, 0}, 16},  // 169.254.0.0/16
    Ipv4Cidr{{172, 16, 0, 0}, 12},   // 172.16.0.0/12
    Ipv4Cidr{{192, 0, 0, 0}, 24},    // 192.0.0.0/24
    Ipv4Cidr{{192, 0, 2, 0}, 24},    // 192.0.2.0/24
    Ipv4Cidr{{192, 88, 99, 0}, 24},  // 192.88.99.0/24
    Ipv4Cidr{{192, 168, 0, 0}, 16},  // 192.168.0.0/16
    Ipv4Cidr{{198, 18, 0, 0}, 15},   // 198.18.0.0/15
    Ipv4Cidr{{198, 51, 100, 0}, 24}, // 198.51.100.0/24
    Ipv4Cidr{{203, 0, 113, 0}, 24},  // 203.0.113.0/24
    Ipv4Cidr{{224, 0, 0, 0}, 3},     // 224.0.0.0/3
};

} // namespace

bool is_public_ipv4(std::uint32_t network_order) noexcept {
    const auto address = ntohl(network_order);
    return std::ranges::none_of(kNonPublicIpv4Ranges, [address](const auto& range) {
        return range.contains(address);
    });
}

std::string_view format_endpoint(const Endpoint& endpoint, std::span<char> output) noexcept {
    if (!endpoint.valid() || output.empty()) {
        return {};
    }
    const auto address = ntohl(endpoint.ipv4_network_order);
    const auto written =
        std::snprintf(output.data(), output.size(), "%u.%u.%u.%u:%u", address >> 24, (address >> 16) & 0xFF,
                      (address >> 8) & 0xFF, address & 0xFF, endpoint.port_host_order);
    if (written <= 0) {
        output.front() = '\0';
        return {};
    }
    const auto length = std::min(static_cast<std::size_t>(written), output.size() - 1);
    return {output.data(), length};
}

UdpSocketRuntime::~UdpSocketRuntime() {
    stop();
}

bool UdpSocketRuntime::start(std::uint16_t port, std::int32_t& error) noexcept {
    stop();
    error = 0;
    if (start_win_sock(error) && open_socket(error) && configure_socket(error) && bind_socket(port, error)) {
        return true;
    }
    stop();
    return false;
}

void UdpSocketRuntime::stop() noexcept {
    if (handle_ != kInvalidHandle) {
        closesocket(to_socket(handle_));
        handle_ = kInvalidHandle;
    }
    bound_port_ = 0;
    if (win_sock_started_) {
        WSACleanup();
        win_sock_started_ = false;
    }
}

bool UdpSocketRuntime::available() const noexcept {
    return win_sock_started_ && handle_ != kInvalidHandle && bound_port_ != 0;
}

std::uint16_t UdpSocketRuntime::bound_port() const noexcept {
    return bound_port_;
}

SocketResult UdpSocketRuntime::send(std::span<const std::uint8_t> bytes, const Endpoint& endpoint) noexcept {
    if (!available() || bytes.empty() || !endpoint.valid()) {
        return {-1, kInvalidSocketOperation};
    }
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {-1, WSAEMSGSIZE};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = endpoint.ipv4_network_order;
    address.sin_port = htons(endpoint.port_host_order);
    const auto result =
        sendto(to_socket(handle_), reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    return {result, result == SOCKET_ERROR ? WSAGetLastError() : 0};
}

SocketResult UdpSocketRuntime::receive(std::span<std::uint8_t> bytes, Endpoint& endpoint) noexcept {
    endpoint = {};
    if (!available() || bytes.empty()) {
        return {-1, kInvalidSocketOperation};
    }
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {-1, WSAEMSGSIZE};
    }

    sockaddr_in address{};
    int address_bytes = sizeof(address);
    const auto result =
        recvfrom(to_socket(handle_), reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                 reinterpret_cast<sockaddr*>(&address), &address_bytes);
    if (result == SOCKET_ERROR) {
        return {-1, WSAGetLastError()};
    }
    if (address_bytes < static_cast<int>(sizeof(address))) {
        return {-1, WSAEFAULT};
    }
    if (address.sin_family != AF_INET) {
        return {-1, WSAEAFNOSUPPORT};
    }
    endpoint.ipv4_network_order = address.sin_addr.s_addr;
    endpoint.port_host_order = ntohs(address.sin_port);
    return {result, 0};
}

bool UdpSocketRuntime::start_win_sock(std::int32_t& error) noexcept {
    WSADATA data{};
    error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0) {
        return false;
    }
    if (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
        WSACleanup();
        error = WSAVERNOTSUPPORTED;
        return false;
    }
    win_sock_started_ = true;
    return true;
}

bool UdpSocketRuntime::open_socket(std::int32_t& error) noexcept {
    const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET) {
        error = WSAGetLastError();
        return false;
    }
    handle_ = static_cast<Handle>(handle);
    return true;
}

bool UdpSocketRuntime::configure_socket(std::int32_t& error) noexcept {
    u_long enabled = 1;
    if (ioctlsocket(to_socket(handle_), FIONBIO, &enabled) == SOCKET_ERROR) {
        error = WSAGetLastError();
        return false;
    }
    return set_buffer_option(to_socket(handle_), SO_SNDBUF, kRequestedSendBuffer, error) &&
           set_buffer_option(to_socket(handle_), SO_RCVBUF, kRequestedReceiveBuffer, error);
}

bool UdpSocketRuntime::bind_socket(std::uint16_t port, std::int32_t& error) noexcept {
    sockaddr_in requested{};
    requested.sin_family = AF_INET;
    requested.sin_addr.s_addr = htonl(INADDR_ANY);
    requested.sin_port = htons(port);
    if (bind(to_socket(handle_), reinterpret_cast<const sockaddr*>(&requested), sizeof(requested)) == SOCKET_ERROR) {
        error = WSAGetLastError();
        return false;
    }

    sockaddr_in bound{};
    int bytes = sizeof(bound);
    if (getsockname(to_socket(handle_), reinterpret_cast<sockaddr*>(&bound), &bytes) == SOCKET_ERROR) {
        error = WSAGetLastError();
        return false;
    }
    if (bytes < static_cast<int>(sizeof(bound))) {
        error = WSAEFAULT;
        return false;
    }
    if (bound.sin_family != AF_INET) {
        error = WSAEAFNOSUPPORT;
        return false;
    }
    bound_port_ = ntohs(bound.sin_port);
    if (bound_port_ == 0) {
        error = kInvalidSocketOperation;
        return false;
    }
    return true;
}

} // namespace fusioncutter::patches::direct_transport
