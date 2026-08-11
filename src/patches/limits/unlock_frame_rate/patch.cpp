#include "patch.hpp"

#include "../../categories.hpp"
#include "unlock_frame_rate.hpp"

#include <cstdint>
#include <limits>

namespace fusioncutter::patches::unlock_frame_rate {
namespace {

const PatchVariants kVariants{
    make_patch_variant<UnlockFrameRate, TargetLayout::SteamRetail, UnlockFrameRateSettings>(HostRole::Client,
                                                                                            TargetImage::Game),
    make_patch_variant<UnlockFrameRate, TargetLayout::GOGRetail, UnlockFrameRateSettings>(HostRole::Client,
                                                                                          TargetImage::Game),
    make_patch_variant<UnlockFrameRate, TargetLayout::ModTools, UnlockFrameRateSettings>(HostRole::Client,
                                                                                         TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Unlock Frame Rate",
        .enabled = true,
        .configurable = true,
        .category = categories::Limits,
        .description = "Set the game's maximum frame rate.",
        .settings = SettingsDefinition::from(SettingsSchema<UnlockFrameRateSettings>{
            .values = {setting("MaxFrameRate", &UnlockFrameRateSettings::max_frame_rate, std::uint32_t{120})
                           .description("Maximum rendered frames per second; 0 removes the limit.")
                           .range(0, std::numeric_limits<std::uint32_t>::max())},
        }),
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::unlock_frame_rate
