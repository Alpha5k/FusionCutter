#include "patch.hpp"

#include <FusionCutter/categories.hpp>
#include "client/direct_transport.hpp"
#include "server/direct_transport.hpp"
#include "server/policy.hpp"

#include <array>

namespace fusioncutter::patches::direct_transport {
namespace {

constexpr std::array<PatchRelationship, 1> kIncludes{
    PatchRelationship{"GalaxyPeerObserver", TargetLayout::GOGRetail, HostRole::Server},
};

constexpr std::array<PatchRelationship, 1> kDependsOn{"NetworkPipeline"};

using ClientPatch = client::DirectTransportClient;
using ServerPatch = server::DirectTransportServer;

const auto kServerSettings = SettingsDefinition::from(SettingsSchema<DirectTransportSettings>{
    .values = {
        choice("Policy", &DirectTransportSettings::policy, Policy::Disabled, server::kPolicyChoices)
            .description("Controls which players must use authenticated direct UDP."),
    }});

const PatchVariants kVariants{
    make_patch_variant<ClientPatch, TargetLayout::GOGRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<ClientPatch, TargetLayout::SteamRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<ServerPatch, TargetLayout::GOGRetail, HostRole::Server, DirectTransportSettings>(
        TargetImage::Game, kServerSettings, StartupFailurePolicy::StartupRequired),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Direct Transport",
        .enabled = true,
        .configurable = true,
        .category = categories::Networking,
        .description = "Sends multiplayer packets directly over authenticated UDP instead of GalaxyPeer",
        .depends_on = kDependsOn,
        .includes = kIncludes,
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::direct_transport
