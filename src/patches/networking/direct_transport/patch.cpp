#include "patch.hpp"

#include "../../categories.hpp"
#include "client/direct_transport.hpp"
#include "server/direct_transport.hpp"
#include "server/policy.hpp"

#include <array>

namespace fusioncutter::patches::direct_transport {
namespace {

constexpr std::array<PatchId, 1> kIncludes{"GalaxyPeerObserver"};

const PatchVariants kVariants{
    make_patch_variant<client::DirectTransportClient, TargetLayout::GOGRetail, NoSettings>(
        HostRole::Client, TargetImage::Game, SettingsDefinition::from(SettingsSchema<NoSettings>{})),
    make_patch_variant<client::DirectTransportClient, TargetLayout::SteamRetail, NoSettings>(
        HostRole::Client, TargetImage::Game, SettingsDefinition::from(SettingsSchema<NoSettings>{})),
    make_patch_variant<server::DirectTransportServer, TargetLayout::GOGRetail, DirectTransportSettings>(
        HostRole::Server, TargetImage::Game, ImageTiming::Startup, StartupFailurePolicy::StartupRequired),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Direct Transport",
        .enabled = true,
        .configurable = true,
        .category = categories::Networking,
        .description = "Sends multiplayer packets directly over authenticated UDP instead of GalaxyPeer",
        .settings = SettingsDefinition::from(SettingsSchema<DirectTransportSettings>{
            .values =
                {
                    choice("Policy", &DirectTransportSettings::policy, Policy::Disabled, server::kPolicyChoices)
                        .description("Controls which players must use authenticated direct UDP."),
                }}),
        .includes = kIncludes,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::direct_transport
