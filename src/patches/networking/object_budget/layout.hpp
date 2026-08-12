#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::object_budget::layout {

inline constexpr std::uint32_t kBudgetRoundingRva = 0x001CE726;
inline constexpr auto kBudgetRounding =
    byte_array<0x99, 0x81, 0xE2, 0xFF, 0x03, 0x00, 0x00, 0x03, 0xC2, 0xC1, 0xF8, 0x0A>();

inline constexpr std::uint32_t kCurrentDestinationRva = 0x01BA9C2C;
inline constexpr std::uint32_t kDestinationEventCursorRva = 0x01ACEF88;
inline constexpr std::uint32_t kEventHeadRva = 0x01BA9C40;
inline constexpr std::uint32_t kNetUpdateSizeRva = 0x003E9268;

} // namespace fusioncutter::patches::object_budget::layout
