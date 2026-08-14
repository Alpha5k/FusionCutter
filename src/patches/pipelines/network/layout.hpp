#pragma once

#include <FusionCutter/patching.hpp>
#include <FusionCutter/target.hpp>

#include <cstdint>
#include <utility>

namespace fusioncutter::patches::network_pipeline {

struct PipelineLayout {
    std::uint32_t final_send_rva;
    std::uint32_t group_send_rva;
    std::uint32_t receive_rva;
    std::uint32_t intake_rva;
    std::uint32_t disconnect_rva;
    std::uint32_t reset_rva;
};

inline constexpr auto kFinalSendPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57, 0x8B, 0xFA, 0x8B, 0xF1>();
inline constexpr auto kGroupSendPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x89, 0x55, 0xF8, 0x33, 0xF6>();
inline constexpr auto kReceivePreimage = byte_array<0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56>();
inline constexpr auto kIntakePreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0xDA, 0x89, 0x5D, 0xFC, 0x0F, 0xBE>();
inline constexpr auto kDisconnectPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0x53, 0x8B, 0xD9, 0xB9>();
inline constexpr auto kResetPreimage =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x08, 0x53, 0x56, 0x8A, 0xD9>();

inline constexpr PipelineLayout kGogLayout{
    .final_send_rva = 0x0021'8EC0,
    .group_send_rva = 0x001B'38A0,
    .receive_rva = 0x001B'4170,
    .intake_rva = 0x001B'4240,
    .disconnect_rva = 0x001B'3A90,
    .reset_rva = 0x001B'33D0,
};

inline constexpr PipelineLayout kSteamLayout{
    .final_send_rva = 0x0021'7E50,
    .group_send_rva = 0x001B'2900,
    .receive_rva = 0x001B'31C0,
    .intake_rva = 0x001B'3290,
    .disconnect_rva = 0x001B'2AF0,
    .reset_rva = 0x001B'2430,
};

[[nodiscard]] inline constexpr const PipelineLayout& layout_for(TargetLayout target) noexcept {
    switch (target) {
    case TargetLayout::GOGRetail:
        return kGogLayout;
    case TargetLayout::SteamRetail:
        return kSteamLayout;
    case TargetLayout::Aspyr:
    case TargetLayout::ModTools:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace fusioncutter::patches::network_pipeline
