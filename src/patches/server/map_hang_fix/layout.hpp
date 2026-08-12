#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::map_hang_fix::layout {

inline constexpr std::uint32_t kReadinessDecisionRva = 0x001B607D;
inline constexpr auto kReadinessDecision =
    byte_array<0x85, 0xC0, 0x75, 0x07, 0xB0, 0x01, 0xE9, 0xC9, 0x00, 0x00, 0x00>();
inline constexpr std::uint32_t kMapStatusRva = 0x01AB1054;

} // namespace fusioncutter::patches::map_hang_fix::layout
