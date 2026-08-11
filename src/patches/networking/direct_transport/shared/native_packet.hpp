#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace fusioncutter::patches::direct_transport {

inline constexpr std::uint8_t kPhysicalAssociationCount = 64;
inline constexpr std::uint8_t kNativeEndpointCount = 67;
inline constexpr std::size_t kNativeEndpointStride = 0x60;
inline constexpr std::size_t kNativeEndpointConnectedOffset = 0x20;
inline constexpr std::size_t kNativeEndpointTableRequiredBytes =
    kNativeEndpointConnectedOffset + (kNativeEndpointCount - 1) * kNativeEndpointStride + sizeof(std::uint64_t);

using NativePacketAllocate = void*(__fastcall*)(bool small);
using NativePacketInitialize = void(__thiscall*)(void* packet);

// Holds the validated game routines and prefix needed to construct an incoming native packet.
struct NativePacketFactory {
    NativePacketAllocate allocate{};
    NativePacketInitialize initialize{};
    const std::uint32_t* prefix_bytes{};
};

enum class NativePacketResult : std::uint8_t {
    Built,
    RejectedPayload,
    FactoryUnavailable,
    InvalidPrefix,
    AllocationFailed,
};

// Reads the Galaxy identity stored in one connected native endpoint slot.
[[nodiscard]] bool read_native_identity(const void* endpoint_table, std::uint8_t physical_primary,
                                        std::uint64_t& identity) noexcept;
// Recreates a game-owned packet from an authenticated direct payload for native intake.
[[nodiscard]] NativePacketResult build_native_packet(const NativePacketFactory& factory,
                                                     std::span<const std::uint8_t> bytes, void*& packet) noexcept;

} // namespace fusioncutter::patches::direct_transport
