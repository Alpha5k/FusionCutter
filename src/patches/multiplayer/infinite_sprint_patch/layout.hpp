#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::infinite_sprint_patch::layout {

inline constexpr std::uint32_t kRollUsingEnergyRva = 0x000EDD30;
inline constexpr std::uint32_t kSprintRollCallRva = 0x000EB146;
inline constexpr auto kSprintRollCall = byte_array<0xE8, 0xE5, 0x2B, 0x00, 0x00>();

inline constexpr std::size_t kControllableOffset = 0x240;
inline constexpr std::size_t kEnergyFlagsOffset = 0xA18;
inline constexpr std::size_t kEndSprintVtableOffset = 0xB0;

} // namespace fusioncutter::patches::infinite_sprint_patch::layout
