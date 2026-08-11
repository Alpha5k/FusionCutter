#pragma once

#include "../recognition.hpp"

#include <array>

namespace fusioncutter::targets::layouts::mod_tools {

// Ghidra Reference /BF2_modtools_NoDVD.exe (program build 8194).
// BF2GameExt's ModTools patch list identifies this layout. The stock
// /BF2_modtools.exe image has different code and is deliberately unsupported.
inline constexpr std::array kGameSections = {
    SectionProfile{".text", 0x00001000, 0x006288AC},
    SectionProfile{".rdata", 0x0062A000, 0x00098F47},
    SectionProfile{".data", 0x006C3000, 0x0188870C},
};

[[nodiscard]] constexpr ImageProfile game_profile(std::string_view basename) {
    return {
        .fingerprint = "ModTools.Game.43EBD102",
        .layout = TargetLayout::ModTools,
        .identity = TargetImage::Game,
        .basename = basename,
        .architecture = Architecture::X86,
        .size_of_image = 0x01F51000,
        .timestamp = std::nullopt,
        .sections = kGameSections,
        .marker = std::nullopt,
    };
}

inline constexpr auto kGame = game_profile("BF2_modtools.exe");

// Preserve the reference artifact's explicit filename as an alias for the same
// physical layout; normal ModTools installations commonly use kGame's basename.
inline constexpr auto kNoDvdNamedGame = game_profile("BF2_modtools_NoDVD.exe");

} // namespace fusioncutter::targets::layouts::mod_tools
