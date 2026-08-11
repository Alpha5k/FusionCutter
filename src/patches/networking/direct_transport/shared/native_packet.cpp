#include "native_packet.hpp"

#include "protocol.hpp"

#include <cstddef>
#include <cstring>

namespace fusioncutter::patches::direct_transport {

bool read_native_identity(const void* endpoint_table, std::uint8_t physical_primary, std::uint64_t& identity) noexcept {
    identity = 0;
    if (endpoint_table == nullptr || physical_primary >= kNativeEndpointCount) {
        return false;
    }

    // Each endpoint record stores its Galaxy identity at +0 and its connected flag 0x20 bytes before the record.
    const auto* record = static_cast<const std::byte*>(endpoint_table) + physical_primary * kNativeEndpointStride;
    if (*reinterpret_cast<const std::uint8_t*>(record - kNativeEndpointConnectedOffset) == 0) {
        return false;
    }
    std::memcpy(&identity, record, sizeof(identity));
    return identity != 0;
}

NativePacketResult build_native_packet(const NativePacketFactory& factory, std::span<const std::uint8_t> bytes,
                                       void*& packet) noexcept {
    packet = nullptr;
    if (bytes.size() < kNativeHeaderBytes || bytes.size() > kMaximumNativeBytes) {
        return NativePacketResult::RejectedPayload;
    }
    if (factory.allocate == nullptr || factory.initialize == nullptr) {
        return NativePacketResult::FactoryUnavailable;
    }

    // Low native message types reserve the game's three-byte prefix before their serialized payload.
    const auto prefix = bytes[0] < 0x1E && factory.prefix_bytes != nullptr ? *factory.prefix_bytes : 0U;
    if ((bytes[0] < 0x1E && prefix != 3) || prefix > 0x74 || bytes.size() > kMaximumNativeBytes - prefix) {
        return NativePacketResult::InvalidPrefix;
    }

    packet = factory.allocate(bytes.size() <= 0x74 - prefix);
    if (packet == nullptr) {
        return NativePacketResult::AllocationFailed;
    }
    factory.initialize(packet);
    auto* packet_bytes = static_cast<std::uint8_t*>(packet);
    std::memcpy(packet_bytes + 0x0C + prefix, bytes.data(), bytes.size());
    packet_bytes[8] = static_cast<std::uint8_t>(prefix);
    const auto total = static_cast<std::uint16_t>(bytes.size() + prefix);
    std::memcpy(packet_bytes + 6, &total, sizeof(total));
    return NativePacketResult::Built;
}

} // namespace fusioncutter::patches::direct_transport
