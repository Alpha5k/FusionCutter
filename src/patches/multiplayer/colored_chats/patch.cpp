#include "patch.hpp"

#include "../../categories.hpp"
#include "colored_chats.hpp"

#include <string>

namespace fusioncutter::patches::colored_chats {
namespace {

const PatchVariants kVariants{
    make_patch_variant<ColoredChats, TargetLayout::SteamRetail, ColoredChatsSettings>(HostRole::Client,
                                                                                      TargetImage::Game),
    make_patch_variant<ColoredChats, TargetLayout::GOGRetail, ColoredChatsSettings>(HostRole::Client,
                                                                                    TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Colored Chats",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Set normal, team, and admin chat colors and keep messages on one line.",
        .settings = SettingsDefinition::from(SettingsSchema<ColoredChatsSettings>{
            .values =
                {
                    setting("DefaultColor", &ColoredChatsSettings::default_color, std::string{"20DFDF"})
                        .description("Six-digit RGB color for normal chat."),
                    setting("TeamColor", &ColoredChatsSettings::team_color, std::string{"20DF20"})
                        .description("Six-digit RGB color for team chat."),
                    setting("AdminColor", &ColoredChatsSettings::admin_color, std::string{"D8D8D8"})
                        .description("Six-digit RGB color for admin chat."),
                },
            .validate = validate_colors,
        }),
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::colored_chats
