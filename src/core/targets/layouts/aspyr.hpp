#pragma once

#include "../recognition.hpp"

#include <array>

namespace fusioncutter::targets::layouts::aspyr {

// Ghidra Master /Patch3/Battlefront.exe.7. This is the Patch 3 bootstrap that
// loads the game DLL; patch RVAs do not belong to this image.
inline constexpr std::array kBootstrapSections = {
    SectionProfile{".text", 0x00001000, 0x000BA5FC},
    SectionProfile{".rdata", 0x000BC000, 0x000316B0},
    SectionProfile{".data", 0x000EE000, 0x00013860},
};

inline constexpr ImageProfile kBootstrap{
    .fingerprint = "Aspyr.Bootstrap.66702CD7",
    .layout = TargetLayout::Aspyr,
    .identity = TargetImage::Bootstrap,
    .basename = "Battlefront.exe",
    .architecture = Architecture::X64,
    .size_of_image = 0x0014E000,
    .timestamp = std::nullopt,
    .sections = kBootstrapSections,
    .marker = std::nullopt,
};

// Ghidra Master /Battlefront2.dll.0 and Reference /Battlefront2_CC.dll.
// These images contain the Patch 3 RVAs used by the existing Aspyr RconServer.
inline constexpr std::array kGameSections = {
    SectionProfile{".text", 0x00001000, 0x004FF19C},
    SectionProfile{".rdata", 0x00501000, 0x001375C0},
    SectionProfile{".data", 0x00639000, 0x01DBC550},
};

[[nodiscard]] constexpr ImageProfile game_profile(std::string_view basename) {
    return {
        .fingerprint = "Aspyr.Game.66702CD2",
        .layout = TargetLayout::Aspyr,
        .identity = TargetImage::Game,
        .basename = basename,
        .architecture = Architecture::X64,
        .size_of_image = 0x0245F000,
        .timestamp = std::nullopt,
        .sections = kGameSections,
        .marker = std::nullopt,
    };
}

inline constexpr auto kGame = game_profile("Battlefront2.dll");

// The client proxy preserves the real Patch 3 DLL under this name. It remains
// the same target image and therefore shares the same fingerprint and layout.
inline constexpr auto kOriginalNamedGame = game_profile("Battlefront2.original.dll");

} // namespace fusioncutter::targets::layouts::aspyr
