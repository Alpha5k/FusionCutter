#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::update_scheduling::layout {

inline constexpr std::uint32_t kDedicatedPresentCallRva = 0x001338FA;
inline constexpr auto kDedicatedPresentCall = byte_array<0xE8, 0x11, 0xA8, 0x18, 0x00>();
inline constexpr std::uint32_t kHalfClientLimiterRva = 0x001C9C19;
inline constexpr auto kHalfClientLimiter = byte_array<0x99, 0x2B, 0xC2, 0xD1, 0xF8>();

inline constexpr std::uint32_t kSentSlotTimeCallRva = 0x001D2DF1;
inline constexpr auto kSentSlotTimeCall = byte_array<0xE8, 0x4A, 0x0A, 0xFE, 0xFF>();
inline constexpr std::uint32_t kSentSlotSkipRva = 0x001D2E21;

inline constexpr std::uint32_t kCreateMarkerRva = 0x001CE582;
inline constexpr auto kCreateMarker = byte_array<0xC7, 0x85, 0x7C, 0xFF, 0xFF, 0xFF, 0x06, 0x00, 0x00, 0x00>();

inline constexpr std::uint32_t kCreateFenceGateRva = 0x001C9D56;
inline constexpr auto kCreateFenceGate = byte_array<0x75, 0x05, 0xE9, 0x3B, 0xFF, 0xFF, 0xFF>();
inline constexpr std::uint32_t kSendDestinationRva = 0x001C9D5D;
inline constexpr std::uint32_t kSkipDestinationRva = 0x001C9C98;

inline constexpr std::uint32_t kNextUpdateTurnRva = 0x001D2E8F;
inline constexpr auto kStockNextUpdateTurn = byte_array<0x03, 0x4D, 0xE8>();
inline constexpr auto kNextServerTurn = byte_array<0x83, 0xC1, 0x01>();

inline constexpr std::uint32_t kCurrentDestinationRva = 0x01BA9C2C;
inline constexpr std::uint32_t kPlayerStatesRva = 0x01ACEF50;
inline constexpr std::uint32_t kNetworkTimeRva = 0x01BA5214;
inline constexpr std::uint32_t kAllocateObjectMapRva = 0x001B5670;
inline constexpr std::uint32_t kFreeObjectMapRva = 0x001B5720;

} // namespace fusioncutter::patches::update_scheduling::layout
