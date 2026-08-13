#include "layout.hpp"

#include <utility>

namespace fusioncutter::patches::weapon_swap_replay {
namespace {

constexpr auto kLocalSelect =
    byte_array<0x6A, 0x00, 0xFF, 0x74, 0x24, 0x24, 0x88, 0x01, 0x0F, 0xBE, 0xC0, 0x8B, 0x8C, 0x87, 0xE0, 0x04>();
constexpr auto kPackedSync =
    byte_array<0x8B, 0x4C, 0x24, 0x38, 0x8A, 0x44, 0x24, 0x57, 0xFF, 0x74, 0x24, 0x30, 0xC0, 0xE0, 0x02, 0x8B>();
constexpr auto kAuthoritativeSelect =
    byte_array<0x8A, 0x02, 0x3A, 0x44, 0x24, 0x1C, 0x74, 0x3B, 0x8B, 0x74, 0x24, 0x10, 0x0F, 0xBE, 0xC0, 0x03>();
constexpr auto kPackedSelect =
    byte_array<0x85, 0xC9, 0x78, 0x0F, 0x8B, 0x8C, 0x8F, 0x20, 0x07, 0x00, 0x00, 0x6A, 0x00, 0x52, 0x8B, 0x01>();
constexpr auto kBaseSelect =
    byte_array<0xF3, 0x0F, 0x11, 0x87, 0xC4, 0x00, 0x00, 0x00, 0x75, 0x2A, 0x8B, 0x43, 0x0C, 0x8D, 0x4B, 0x0C>();
constexpr auto kHudWeapon =
    byte_array<0x8B, 0xF0, 0x89, 0x75, 0xF0, 0x85, 0xF6, 0x74, 0x0D, 0x8B, 0x46, 0x64, 0x89, 0x45, 0xFC, 0xEB>();
constexpr auto kRenderSelection =
    byte_array<0x33, 0xF6, 0x8A, 0xC1, 0x89, 0x74, 0x24, 0x38, 0xC0, 0xE0, 0x04, 0x8A, 0xD0, 0xC6, 0x44, 0x24>();
constexpr auto kRenderChannelSelection =
    byte_array<0x24, 0x30, 0x3C, 0x10, 0x75, 0x62, 0x8A, 0x87, 0xAC, 0x06, 0x00, 0x00, 0x84, 0xC0, 0x78, 0x58>();
constexpr auto kModelSelection =
    byte_array<0x8A, 0xC2, 0xC0, 0xE0, 0x04, 0xA8, 0xF0, 0x0F, 0x8C, 0x27, 0x01, 0x00, 0x00, 0x0F, 0xBE, 0xC0>();
constexpr auto kSwitchLatch =
    byte_array<0xF3, 0x0F, 0x11, 0x8C, 0x91, 0x90, 0x00, 0x00, 0x00, 0x5D, 0xC2, 0x04, 0x00, 0xC7, 0x84, 0x91>();

constexpr auto kUpdateTurnStore = byte_array<0xA1, 0x00, 0x00, 0x00, 0x00, 0xA3, 0x00, 0x00, 0x00, 0x00>();
constexpr auto kPredictTurnReader =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x83, 0xE0, 0x1F, 0x89, 0x45>();
constexpr auto kAcknowledgedTurnStore =
    byte_array<0xA1, 0x00, 0x00, 0x00, 0x00, 0x2B, 0xC2, 0xA3, 0x00, 0x00, 0x00, 0x00>();
constexpr auto kNetworkStateGuard =
    byte_array<0x8A, 0x15, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x05, 0x00,
               0x00, 0x00, 0x00, 0x0F, 0xB6, 0xCA, 0x0F, 0x45, 0xC8, 0x84, 0xC9, 0x74, 0x4F, 0x84, 0xD2, 0x74, 0x4B,
               0x80, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00>();
constexpr auto kClientTurnSetter = byte_array<0x55, 0x8B, 0xEC, 0x83, 0x7D, 0x08, 0x00, 0x76, 0x7D, 0x8B, 0x45, 0x08,
                                              0xA3, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00>();
constexpr auto kMoveHistoryAccess =
    byte_array<0x8D, 0x84, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x4D, 0xF0, 0x8B, 0x10, 0x89, 0x11>();

constexpr auto kSteamJoystickLookup =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0xE8, 0x25, 0xB3, 0xFF, 0xFF, 0x5D, 0xC3>();
constexpr auto kGogJoystickLookup =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0xE8, 0x15, 0xB3, 0xFF, 0xFF, 0x5D, 0xC3>();
constexpr auto kSteamPrimaryCaller = byte_array<0xE8, 0x76, 0x8E, 0xE6, 0xFF>();
constexpr auto kSteamSecondaryCaller = byte_array<0xE8, 0x5E, 0x8E, 0xE6, 0xFF>();
constexpr auto kGogPrimaryCaller = byte_array<0xE8, 0x06, 0x7E, 0xE6, 0xFF>();
constexpr auto kGogSecondaryCaller = byte_array<0xE8, 0xEE, 0x7D, 0xE6, 0xFF>();

constexpr WeaponSwapLayout kSteamLayout{
    .joystick_lookup = {.rva = 0x001B73C0, .expected = kSteamJoystickLookup},
    .state =
        {
            .update_turn_rva = 0x01BA65B0,
            .predict_turn_rva = 0x01BA87B4,
            .acknowledged_turn_rva = 0x01BA6594,
            .network_enabled_rva = 0x01A62EA9,
            .network_fallback_rva = 0x01A62EA8,
            .network_override_rva = 0x003E8007,
            .network_client_active_rva = 0x01A62EAB,
            .client_turn_rva = 0x01A64BA4,
            .local_move_history_rva = 0x01AE4B40,
            .client_host_turn_rva = 0x01A64DE4,
            .update_turn_store = {.rva = 0x001BB6C0, .expected = kUpdateTurnStore},
            .predict_turn_reader = {.rva = 0x001BE3A0, .expected = kPredictTurnReader},
            .acknowledged_turn_store = {.rva = 0x001BBA39, .expected = kAcknowledgedTurnStore},
            .network_state_guard = {.rva = 0x00083E75, .expected = kNetworkStateGuard},
            .client_turn_setter = {.rva = 0x001B8700, .expected = kClientTurnSetter},
            .move_history_access = {.rva = 0x001BE411, .expected = kMoveHistoryAccess},
        },
    .hooks =
        {
            .local_select = {.rva = 0x000EC839, .expected = kLocalSelect},
            .packed_sync = {.rva = 0x000EA2B5, .expected = kPackedSync},
            .authoritative_select = {.rva = 0x001EABD7, .expected = kAuthoritativeSelect},
            .packed_select = {.rva = 0x001EA3CE, .expected = kPackedSelect},
            .base_select = {.rva = 0x002780BA, .expected = kBaseSelect},
            .hud_weapon = {.rva = 0x00160AF3, .expected = kHudWeapon},
            .render_selection = {.rva = 0x000E301D, .expected = kRenderSelection},
            .render_channel_selection = {.rva = 0x000E3075, .expected = kRenderChannelSelection},
            .model_selection = {.rva = 0x000E4CB4, .expected = kModelSelection},
            .switch_latch = {.rva = 0x000839F1, .expected = kSwitchLatch},
            .switch_primary_caller = {.rva = 0x0021AB65, .expected = kSteamPrimaryCaller},
            .switch_secondary_caller = {.rva = 0x0021AB7D, .expected = kSteamSecondaryCaller},
        },
};

constexpr WeaponSwapLayout kGogLayout{
    .joystick_lookup = {.rva = 0x001B8370, .expected = kGogJoystickLookup},
    .state =
        {
            .update_turn_rva = 0x01BA7A64,
            .predict_turn_rva = 0x01BA9C68,
            .acknowledged_turn_rva = 0x01BA7A48,
            .network_enabled_rva = 0x01A64359,
            .network_fallback_rva = 0x01A64358,
            .network_override_rva = 0x003E9007,
            .network_client_active_rva = 0x01A6435B,
            .client_turn_rva = 0x01A66054,
            .local_move_history_rva = 0x01AE5FF0,
            .client_host_turn_rva = 0x01A66294,
            .update_turn_store = {.rva = 0x001BC670, .expected = kUpdateTurnStore},
            .predict_turn_reader = {.rva = 0x001BF330, .expected = kPredictTurnReader},
            .acknowledged_turn_store = {.rva = 0x001BC9E9, .expected = kAcknowledgedTurnStore},
            .network_state_guard = {.rva = 0x00083E75, .expected = kNetworkStateGuard},
            .client_turn_setter = {.rva = 0x001B96B0, .expected = kClientTurnSetter},
            .move_history_access = {.rva = 0x001BF3A1, .expected = kMoveHistoryAccess},
        },
    .hooks =
        {
            .local_select = {.rva = 0x000EC839, .expected = kLocalSelect},
            .packed_sync = {.rva = 0x000EA2B5, .expected = kPackedSync},
            .authoritative_select = {.rva = 0x001EBC77, .expected = kAuthoritativeSelect},
            .packed_select = {.rva = 0x001EB46E, .expected = kPackedSelect},
            .base_select = {.rva = 0x0027915A, .expected = kBaseSelect},
            .hud_weapon = {.rva = 0x00161873, .expected = kHudWeapon},
            .render_selection = {.rva = 0x000E301D, .expected = kRenderSelection},
            .render_channel_selection = {.rva = 0x000E3075, .expected = kRenderChannelSelection},
            .model_selection = {.rva = 0x000E4CB4, .expected = kModelSelection},
            .switch_latch = {.rva = 0x000839F1, .expected = kSwitchLatch},
            .switch_primary_caller = {.rva = 0x0021BBD5, .expected = kGogPrimaryCaller},
            .switch_secondary_caller = {.rva = 0x0021BBED, .expected = kGogSecondaryCaller},
        },
};

} // namespace

const WeaponSwapLayout& layout_for(TargetLayout target) noexcept {
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

void add_layout_requirements(PatchPlan& plan, const ImageContext& image, const WeaponSwapLayout& layout) {
    auto update_store = layout.state.update_turn_store.expected;
    embed_image_address<1>(update_store, image, layout.state.client_host_turn_rva);
    embed_image_address<6>(update_store, image, layout.state.update_turn_rva);

    auto predict_reader = layout.state.predict_turn_reader.expected;
    embed_image_address<7>(predict_reader, image, layout.state.predict_turn_rva);

    auto acknowledged_store = layout.state.acknowledged_turn_store.expected;
    embed_image_address<1>(acknowledged_store, image, layout.state.client_host_turn_rva);
    embed_image_address<8>(acknowledged_store, image, layout.state.acknowledged_turn_rva);

    auto network_guard = layout.state.network_state_guard.expected;
    embed_image_address<2>(network_guard, image, layout.state.network_enabled_rva);
    embed_image_address<8>(network_guard, image, layout.state.network_override_rva);
    embed_image_address<16>(network_guard, image, layout.state.network_fallback_rva);
    embed_image_address<36>(network_guard, image, layout.state.network_client_active_rva);

    auto client_turn_setter = layout.state.client_turn_setter.expected;
    embed_image_address<13>(client_turn_setter, image, layout.state.client_turn_rva);
    embed_image_address<19>(client_turn_setter, image, layout.state.client_turn_rva);

    auto move_history = layout.state.move_history_access.expected;
    embed_image_address<3>(move_history, image, layout.state.local_move_history_rva);

    plan.require_bytes("Validate local-player lookup", layout.joystick_lookup.rva, layout.joystick_lookup.pattern());
    plan.require_bytes("Validate authoritative update frontier", layout.state.update_turn_store.rva,
                       BytePattern::exact(update_store));
    plan.require_bytes("Validate predicted turn frontier", layout.state.predict_turn_reader.rva,
                       BytePattern::exact(predict_reader));
    plan.require_bytes("Validate acknowledged turn frontier", layout.state.acknowledged_turn_store.rva,
                       BytePattern::exact(acknowledged_store));
    plan.require_bytes("Validate client network state", layout.state.network_state_guard.rva,
                       BytePattern::exact(network_guard));
    plan.require_bytes("Validate client turn state", layout.state.client_turn_setter.rva,
                       BytePattern::exact(client_turn_setter));
    plan.require_bytes("Validate local input history", layout.state.move_history_access.rva,
                       BytePattern::exact(move_history));
    plan.require_bytes("Validate primary weapon-switch caller", layout.hooks.switch_primary_caller.rva,
                       layout.hooks.switch_primary_caller.pattern());
    plan.require_bytes("Validate secondary weapon-switch caller", layout.hooks.switch_secondary_caller.rva,
                       layout.hooks.switch_secondary_caller.pattern());
}

} // namespace fusioncutter::patches::weapon_swap_replay
