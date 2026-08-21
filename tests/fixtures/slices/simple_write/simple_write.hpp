#pragma once

#include "simple_write_constants.hpp"

#include <FusionCutter/SDK.hpp>

namespace fc::fixtures::simple_write {

// Each reviewed target keeps the fixture value in writable image data rather than executable instructions.
[[nodiscard]] constexpr fc::Rva value_rva(fc::TargetInfo target) noexcept {
    // Purpose-built native hosts use the same fixed data section while retaining coherent production target tuples.
    if (target.image_profile == "SliceHost_Game_X86") {
        return {0x6000};
    }
    if (target.image_profile == "SliceHost_Game_X64") {
        return {0x7000};
    }
    if (target.layout == fc::TargetLayout::SteamRetail) {
        return {0x3de100};
    }
    if (target.layout == fc::TargetLayout::GOGRetail) {
        return {0x3df100};
    }
    if (target.layout == fc::TargetLayout::ClassicCollection) {
        return {0x639100};
    }
    return {};
}

// Builds the one logical contribution used unchanged by external and bundled acquisition tests.
[[nodiscard]] fc::Plugin build_plugin();

} // namespace fc::fixtures::simple_write
