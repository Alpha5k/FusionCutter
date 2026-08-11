#pragma once

#include "protocol.hpp"
#include "security.hpp"
#include "socket.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fusioncutter::patches::direct_transport {

// Serializes authenticated Direct data and proof datagrams.
[[nodiscard]] std::size_t write_data_datagram(std::span<std::uint8_t> output, Direction direction,
                                              const SessionKey& session_key, std::uint32_t connection_id,
                                              std::uint32_t sequence, std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] bool write_probe_datagram(std::span<std::uint8_t> output, DirectKind kind, Direction direction,
                                        const SessionKey& session_key, std::uint32_t connection_id,
                                        std::uint32_t sequence) noexcept;

// Serializes and sends one native game packet while preserving native send-result semantics.
[[nodiscard]] NativeTransmitResult send_direct_data(UdpSocketRuntime& socket, std::span<std::uint8_t> output,
                                                    Direction direction, const SessionKey& session_key,
                                                    std::uint32_t connection_id, std::uint32_t sequence,
                                                    const Endpoint& endpoint,
                                                    std::span<const std::uint8_t> payload) noexcept;

} // namespace fusioncutter::patches::direct_transport
