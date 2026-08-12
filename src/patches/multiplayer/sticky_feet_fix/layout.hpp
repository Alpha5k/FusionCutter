#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::sticky_feet_fix::layout {

inline constexpr std::uint32_t kJumpUsingEnergyRva = 0x000EDC60;
inline constexpr std::uint32_t kPrimaryJumpCallRva = 0x000EAEA2;
inline constexpr auto kPrimaryJumpCall = byte_array<0xE8, 0xB9, 0x2D, 0x00, 0x00>();
inline constexpr std::uint32_t kSecondaryJumpCallRva = 0x000EB15C;
inline constexpr auto kSecondaryJumpCall = byte_array<0xE8, 0xFF, 0x2A, 0x00, 0x00>();
inline constexpr std::uint32_t kThresholdEpsilonRva = 0x003B2DFC;

inline constexpr std::size_t kControllableOffset = 0x240;
inline constexpr std::size_t kForwardOffset = 0x110;
inline constexpr std::size_t kVelocityOffset = 0x4DC;
inline constexpr std::size_t kSoldierClassOffset = 0x440;
inline constexpr std::size_t kEnergyFlagsOffset = 0xA18;
inline constexpr std::size_t kNormalSpeedOffset = 0x69C;
inline constexpr std::size_t kNormalJumpCostOffset = 0x7B4;
inline constexpr std::size_t kSprintJumpCostOffset = 0x7B8;
inline constexpr std::size_t kApplyJumpVtableOffset = 0xA4;

} // namespace fusioncutter::patches::sticky_feet_fix::layout
