#include "datagram.hpp"

#include <WinSock2.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace fusioncutter::patches::direct_transport {

std::size_t write_data_datagram(std::span<std::uint8_t> output, Direction direction, const SessionKey& session_key,
                                std::uint32_t connection_id, std::uint32_t sequence,
                                std::span<const std::uint8_t> payload) noexcept {
    const auto datagram_bytes = kDirectHeaderBytes + payload.size();
    if (payload.size() < kNativeHeaderBytes || payload.size() > kMaximumNativeBytes || output.size() < datagram_bytes) {
        return 0;
    }

    const auto datagram = output.first(datagram_bytes);
    static_cast<void>(write_direct_header(datagram, DirectKind::Data, connection_id, sequence, 0));
    std::memcpy(datagram.data() + kDirectHeaderBytes, payload.data(), payload.size());
    const auto tag = compute_authentication_tag(direction, session_key, datagram.first(kDirectHeaderBytes),
                                                datagram.subspan(kDirectHeaderBytes));
    store_big_endian64(datagram.data() + offsetof(DirectHeaderV1, authentication_tag), tag);
    return datagram_bytes;
}

bool write_probe_datagram(std::span<std::uint8_t> output, DirectKind kind, Direction direction,
                          const SessionKey& session_key, std::uint32_t connection_id, std::uint32_t sequence) noexcept {
    if ((kind != DirectKind::Probe && kind != DirectKind::ProbeAck) || output.size() < kProbeDatagramBytes) {
        return false;
    }

    const auto datagram = output.first(kProbeDatagramBytes);
    std::ranges::fill(datagram, std::uint8_t{});
    static_cast<void>(write_direct_header(datagram, kind, connection_id, sequence, 0));
    const auto tag = compute_authentication_tag(direction, session_key, datagram.first(kDirectHeaderBytes),
                                                datagram.subspan(kDirectHeaderBytes));
    store_big_endian64(datagram.data() + offsetof(DirectHeaderV1, authentication_tag), tag);
    return true;
}

NativeTransmitResult send_direct_data(UdpSocketRuntime& socket, std::span<std::uint8_t> output, Direction direction,
                                      const SessionKey& session_key, std::uint32_t connection_id,
                                      std::uint32_t sequence, const Endpoint& endpoint,
                                      std::span<const std::uint8_t> payload) noexcept {
    NativeTransmitResult result{.handled = true};
    const auto datagram_bytes = write_data_datagram(output, direction, session_key, connection_id, sequence, payload);
    if (datagram_bytes == 0) {
        result.result = -1;
        result.error = WSAEMSGSIZE;
        result.failure = NativeTransmitResult::Failure::Serialization;
        return result;
    }

    const auto datagram = output.first(datagram_bytes);
    const auto sent = socket.send(datagram, endpoint);
    if (!sent.succeeded()) {
        result.result = -1;
        result.error = sent.error;
        result.failure = NativeTransmitResult::Failure::Socket;
    } else if (sent.value != static_cast<std::int32_t>(datagram_bytes)) {
        result.result = -1;
        result.error = WSAEMSGSIZE;
        result.failure = NativeTransmitResult::Failure::PartialSend;
    } else {
        result.result = static_cast<std::int32_t>(payload.size());
    }
    return result;
}

} // namespace fusioncutter::patches::direct_transport
