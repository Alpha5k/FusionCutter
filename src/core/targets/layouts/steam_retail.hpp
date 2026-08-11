#pragma once

#include "../recognition.hpp"

#include <array>

namespace fusioncutter::targets::layouts::steam_retail {

// Ghidra Master and Reference: /BattlefrontII_Steam.exe.
// This is the layout used by BF2GameExt's Steam table and client_patch's
// steam-galaxy-59ede353-x86 patch profiles.
inline constexpr std::array kGameSections = {
    SectionProfile{".text", 0x00001000, 0x0036950B},
    SectionProfile{".rdata", 0x0036B000, 0x000726A8},
    SectionProfile{".data", 0x003DE000, 0x017CE3C8},
};

inline constexpr ImageProfile kGame{
    .fingerprint = "SteamRetail.Game.59EDE353",
    .layout = TargetLayout::SteamRetail,
    .identity = TargetImage::Game,
    .basename = "BattlefrontII.exe",
    .architecture = Architecture::X86,
    .size_of_image = 0x01BE4000,
    .timestamp = std::nullopt,
    .sections = kGameSections,
    .marker = std::nullopt,
};

} // namespace fusioncutter::targets::layouts::steam_retail
