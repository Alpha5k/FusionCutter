#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::soldier_state_pipeline {

struct SoldierStateLayout {
    NativeSite<16> read;
};

inline constexpr auto kReadPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00, 0x56, 0x57, 0x8B, 0xF9>();

inline constexpr SoldierStateLayout kSteamLayout{
    .read = {.rva = 0x001E'A140, .expected = kReadPreimage},
};

inline constexpr SoldierStateLayout kGogLayout{
    .read = {.rva = 0x001E'B1E0, .expected = kReadPreimage},
};

[[nodiscard]] inline const SoldierStateLayout& layout_for(TargetLayout target) noexcept {
    return target == TargetLayout::SteamRetail ? kSteamLayout : kGogLayout;
}

} // namespace fusioncutter::patches::soldier_state_pipeline
