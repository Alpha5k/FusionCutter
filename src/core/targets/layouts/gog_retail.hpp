#pragma once

#include "../recognition.hpp"

#include <array>

namespace fusioncutter::targets::layouts::gog_retail {

// Ghidra Master /BattlefrontII.exe and Reference /BattlefrontII_GOG.exe.
// This is the current Galaxy retail layout used by BF2GameExt, client_patch,
// and the x86 RconServer patches. The older GOG 1.1/GameSpy layout is distinct.
inline constexpr std::array kGameSections = {
    SectionProfile{".text", 0x00001000, 0x0036A7BB},
    SectionProfile{".rdata", 0x0036C000, 0x00072E46},
    SectionProfile{".data", 0x003DF000, 0x017CE878},
};

inline constexpr ImageProfile kGame{
    .fingerprint = "GOGRetail.Game.59EDF52B",
    .layout = TargetLayout::GOGRetail,
    .identity = TargetImage::Game,
    .basename = "BattlefrontII.exe",
    .architecture = Architecture::X86,
    .size_of_image = 0x01BE5000,
    .timestamp = std::nullopt,
    .sections = kGameSections,
    .marker = std::nullopt,
};

// Ghidra Reference /Galaxy/GalaxyPeer.dll. This is the bundled 2017 image
// supported by the GalaxyPeer endpoint-observer patch.
inline constexpr std::array kGalaxyPeer2017Sections = {
    SectionProfile{".text", 0x00001000, 0x0084BE16},
    SectionProfile{".rdata", 0x0084D000, 0x0017AFA0},
    SectionProfile{".data", 0x009C8000, 0x00088320},
};

inline constexpr ImageProfile kGalaxyPeer2017{
    .fingerprint = "GOGRetail.GalaxyPeer.59E6304A",
    .layout = TargetLayout::GOGRetail,
    .identity = TargetImage::GalaxyPeer,
    .basename = "GalaxyPeer.dll",
    .architecture = Architecture::X86,
    .size_of_image = 0x00AC7000,
    .timestamp = std::nullopt,
    .sections = kGalaxyPeer2017Sections,
    .marker = std::nullopt,
};

// Ghidra Reference /Galaxy/GalaxyPeer.dll.0. This is the 2018 ProgramData
// image supported by the same endpoint-observer patch.
inline constexpr std::array kGalaxyPeer2018Sections = {
    SectionProfile{".text", 0x00001000, 0x00883E65},
    SectionProfile{".rdata", 0x00885000, 0x0018DA68},
    SectionProfile{".data", 0x00A13000, 0x00076550},
};

inline constexpr ImageProfile kGalaxyPeer2018{
    .fingerprint = "GOGRetail.GalaxyPeer.5BBE22A6",
    .layout = TargetLayout::GOGRetail,
    .identity = TargetImage::GalaxyPeer,
    .basename = "GalaxyPeer.dll",
    .architecture = Architecture::X86,
    .size_of_image = 0x00AF3000,
    .timestamp = std::nullopt,
    .sections = kGalaxyPeer2018Sections,
    .marker = std::nullopt,
};

} // namespace fusioncutter::targets::layouts::gog_retail
