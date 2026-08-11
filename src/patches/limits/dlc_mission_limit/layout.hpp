#pragma once

#include <FusionCutter/target.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace fusioncutter::patches::dlc_mission_limit {

struct MissionPointerSite {
    std::string_view operation;
    std::uint32_t retail_rva;
    std::uint32_t mod_tools_rva;
    std::size_t table_offset;
};

inline constexpr std::array kMissionPointerSites{
    MissionPointerSite{"Redirect SetCurrentMap storage", 0x0008EB28, 0x0004935C, 0x000},
    MissionPointerSite{"Redirect SetCurrentMission storage", 0x0008EB68, 0x000493AC, 0x004},
    MissionPointerSite{"Redirect GetContentDirectory storage", 0x0008EBB4, 0x00049415, 0x008},
    MissionPointerSite{"Redirect IsMissionDownloaded storage", 0x0008EBCE, 0x00049472, 0x004},
    MissionPointerSite{"Redirect AddDownloadableContent map", 0x0008EA9F, 0x0004951F, 0x000},
    MissionPointerSite{"Redirect AddDownloadableContent mission", 0x0008EAC3, 0x00049542, 0x004},
    MissionPointerSite{"Redirect AddDownloadableContent directory", 0x0008EAC9, 0x00049548, 0x008},
    MissionPointerSite{"Redirect AddDownloadableContent loaded flag", 0x0008EAF0, 0x00049571, 0x10B},
    MissionPointerSite{"Redirect AddDownloadableContent source flag", 0x0008EAF7, 0x0004957D, 0x10C},
};

[[nodiscard]] constexpr std::uint32_t site_rva(const MissionPointerSite& site, TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
    case TargetLayout::GOGRetail:
        return site.retail_rva;
    case TargetLayout::ModTools:
        return site.mod_tools_rva;
    case TargetLayout::Aspyr:
        std::unreachable();
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::uint32_t limit_rva(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
    case TargetLayout::GOGRetail:
        return 0x0008EA7D;
    case TargetLayout::ModTools:
        return 0x000494FB;
    case TargetLayout::Aspyr:
        std::unreachable();
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::uint32_t legacy_table_rva(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
        return 0x01A30950;
    case TargetLayout::GOGRetail:
        return 0x01A31F00;
    case TargetLayout::ModTools:
        return 0x00708308;
    case TargetLayout::Aspyr:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace fusioncutter::patches::dlc_mission_limit
