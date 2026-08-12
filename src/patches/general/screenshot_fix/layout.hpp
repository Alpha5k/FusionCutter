#pragma once

#include <FusionCutter/patching.hpp>
#include <FusionCutter/target.hpp>

#include <cstdint>
#include <span>
#include <utility>

namespace fusioncutter::patches::screenshot_fix {

struct ScreenshotLayout {
    std::uint32_t request_call_rva;
    std::span<const std::byte> request_call;
    std::uint32_t device_slot_rva;
    std::uint32_t device_reference_rva;
    std::span<const std::byte> device_reference;
};

namespace layout {

inline constexpr auto kSteamRequestCall = byte_array<0xE8, 0x1B, 0x08, 0x1C, 0x00>();
inline constexpr auto kSteamDeviceReference =
    byte_array<0xA1, 0x4C, 0x59, 0x7F, 0x00, 0x50, 0x8B, 0x08, 0xFF, 0x91, 0xA4, 0x00, 0x00, 0x00>();
inline constexpr ScreenshotLayout kSteam{
    0x00133520, kSteamRequestCall, 0x003F594C, 0x002B0560, kSteamDeviceReference,
};

inline constexpr auto kGogRequestCall = byte_array<0xE8, 0x7B, 0x0B, 0x1C, 0x00>();
inline constexpr auto kGogDeviceReference =
    byte_array<0xA1, 0xEC, 0x6D, 0x7F, 0x00, 0x50, 0x8B, 0x08, 0xFF, 0x91, 0xA4, 0x00, 0x00, 0x00>();
inline constexpr ScreenshotLayout kGog{
    0x00134290, kGogRequestCall, 0x003F6DEC, 0x002B15E0, kGogDeviceReference,
};

} // namespace layout

[[nodiscard]] constexpr const ScreenshotLayout& layout_for(TargetLayout target) noexcept {
    switch (target) {
    case TargetLayout::SteamRetail:
        return layout::kSteam;
    case TargetLayout::GOGRetail:
        return layout::kGog;
    case TargetLayout::Aspyr:
    case TargetLayout::ModTools:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace fusioncutter::patches::screenshot_fix
