#pragma once

#include <FusionCutter/target.hpp>

#include <cstdint>

namespace fusioncutter::patches::aerial_gravity {

struct AerialGravityLayout {
    std::uint32_t gravity_operand_rva;
    std::uint32_t turn_time_operand_rva;
    std::uint32_t stock_gravity_rva;
    std::uint32_t outer_time_rva;
    std::uint32_t turn_time_rva;
};

inline constexpr AerialGravityLayout kSteamLayout{
    .gravity_operand_rva = 0x00288D2E,
    .turn_time_operand_rva = 0x00288D36,
    .stock_gravity_rva = 0x003B22C4,
    .outer_time_rva = 0x01A56058,
    .turn_time_rva = 0x01A56054,
};

inline constexpr AerialGravityLayout kGogLayout{
    .gravity_operand_rva = 0x00289DAE,
    .turn_time_operand_rva = 0x00289DB6,
    .stock_gravity_rva = 0x003B323C,
    .outer_time_rva = 0x01A574F0,
    .turn_time_rva = 0x01A574EC,
};

[[nodiscard]] constexpr const AerialGravityLayout& layout_for(TargetLayout target) noexcept {
    switch (target) {
    case TargetLayout::SteamRetail:
        return kSteamLayout;
    case TargetLayout::GOGRetail:
        return kGogLayout;
    case TargetLayout::Aspyr:
    case TargetLayout::ModTools:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace fusioncutter::patches::aerial_gravity
