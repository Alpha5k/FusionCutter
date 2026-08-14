#include "layout.hpp"

#include <utility>

namespace fusioncutter::patches::hero_animation_fix {
namespace {

constexpr auto kInputQueueCall =
    byte_array<0x0F, 0xB6, 0x83, 0x78, 0x01, 0x00, 0x00, 0x8D, 0x8B, 0x34, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x93,
               0x30, 0x01, 0x00, 0x00, 0x50, 0xE8, 0x7A, 0x1F, 0x00, 0x00, 0x8B, 0x8B, 0x80, 0x01, 0x00>();

constexpr auto kSteamJoystickLookup =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0xE8, 0x25, 0xB3, 0xFF, 0xFF, 0x5D, 0xC3>();
constexpr auto kGogJoystickLookup =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0xE8, 0x15, 0xB3, 0xFF, 0xFF, 0x5D, 0xC3>();

constexpr auto kLocalTurnContext =
    byte_array<0x8A, 0x4D, 0xD4, 0x88, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x15, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x15,
               0x00, 0x00, 0x00, 0x00, 0x75, 0x09, 0xC7, 0x45, 0xD0, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x07>();
constexpr auto kUpdateTurnContext =
    byte_array<0xC7, 0x45, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x8A, 0x45, 0xD0, 0xA2, 0x00, 0x00, 0x00, 0x00, 0xF3>();
constexpr auto kAcknowledgedTurnStore =
    byte_array<0xA1, 0x00, 0x00, 0x00, 0x00, 0x2B, 0xC2, 0xA3, 0x00, 0x00, 0x00, 0x00>();
constexpr auto kNetworkStateGuard =
    byte_array<0x8A, 0x15, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x05, 0x00,
               0x00, 0x00, 0x00, 0x0F, 0xB6, 0xCA, 0x0F, 0x45, 0xC8, 0x84, 0xC9, 0x74, 0x4F, 0x84, 0xD2, 0x74, 0x4B,
               0x80, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00>();

constexpr HeroAnimationLayout kSteamLayout{
    .joystick_lookup = {.rva = 0x001B73C0, .expected = kSteamJoystickLookup},
    .native =
        {
            .input_queue_call = {.rva = 0x0028946B, .expected = kInputQueueCall},
            .prediction_resume_rva = 0x00289C17,
        },
    .state =
        {
            .prediction_turn_rva = 0x01BA87B4,
            .acknowledged_turn_rva = 0x01BA6594,
            .is_local_turn_rva = 0x01A62F10,
            .is_update_turn_rva = 0x01A64A2C,
            .update_turn_rva = 0x01BA65B0,
            .client_host_turn_rva = 0x01A64DE4,
            .network_enabled_rva = 0x01A62EA9,
            .network_fallback_rva = 0x01A62EA8,
            .network_override_rva = 0x003E8007,
            .network_client_active_rva = 0x01A62EAB,
            .local_turn_context = {.rva = 0x001BAA6D, .expected = kLocalTurnContext},
            .update_turn_context = {.rva = 0x001BAA8D, .expected = kUpdateTurnContext},
            .acknowledged_turn_store = {.rva = 0x001BBA39, .expected = kAcknowledgedTurnStore},
            .network_state_guard = {.rva = 0x00083E75, .expected = kNetworkStateGuard},
        },
};

constexpr HeroAnimationLayout kGogLayout{
    .joystick_lookup = {.rva = 0x001B8370, .expected = kGogJoystickLookup},
    .native =
        {
            .input_queue_call = {.rva = 0x0028A4FB, .expected = kInputQueueCall},
            .prediction_resume_rva = 0x0028ACA7,
        },
    .state =
        {
            .prediction_turn_rva = 0x01BA9C68,
            .acknowledged_turn_rva = 0x01BA7A48,
            .is_local_turn_rva = 0x01A643C0,
            .is_update_turn_rva = 0x01A65EDC,
            .update_turn_rva = 0x01BA7A64,
            .client_host_turn_rva = 0x01A66294,
            .network_enabled_rva = 0x01A64359,
            .network_fallback_rva = 0x01A64358,
            .network_override_rva = 0x003E9007,
            .network_client_active_rva = 0x01A6435B,
            .local_turn_context = {.rva = 0x001BBA1D, .expected = kLocalTurnContext},
            .update_turn_context = {.rva = 0x001BBA3D, .expected = kUpdateTurnContext},
            .acknowledged_turn_store = {.rva = 0x001BC9E9, .expected = kAcknowledgedTurnStore},
            .network_state_guard = {.rva = 0x00083E75, .expected = kNetworkStateGuard},
        },
};

} // namespace

const HeroAnimationLayout& layout_for(TargetLayout target) noexcept {
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

void add_layout_requirements(PatchPlan& plan, const ImageContext& image, const HeroAnimationLayout& layout) {
    auto local_turn = layout.state.local_turn_context.expected;
    embed_image_address<5>(local_turn, image, layout.state.is_local_turn_rva);
    embed_image_address<11>(local_turn, image, layout.state.prediction_turn_rva);
    embed_image_address<17>(local_turn, image, layout.state.update_turn_rva);

    auto update_turn = layout.state.update_turn_context.expected;
    embed_image_address<11>(update_turn, image, layout.state.is_update_turn_rva);

    auto acknowledged_turn = layout.state.acknowledged_turn_store.expected;
    embed_image_address<1>(acknowledged_turn, image, layout.state.client_host_turn_rva);
    embed_image_address<8>(acknowledged_turn, image, layout.state.acknowledged_turn_rva);

    auto network_state = layout.state.network_state_guard.expected;
    embed_image_address<2>(network_state, image, layout.state.network_enabled_rva);
    embed_image_address<8>(network_state, image, layout.state.network_override_rva);
    embed_image_address<16>(network_state, image, layout.state.network_fallback_rva);
    embed_image_address<36>(network_state, image, layout.state.network_client_active_rva);

    plan.require_bytes("Validate local-player lookup", layout.joystick_lookup.rva, layout.joystick_lookup.pattern());
    plan.require_bytes("Validate prediction-pass context", layout.state.local_turn_context.rva,
                       BytePattern::exact(local_turn));
    plan.require_bytes("Validate authority-pass context", layout.state.update_turn_context.rva,
                       BytePattern::exact(update_turn));
    plan.require_bytes("Validate acknowledged prediction frontier", layout.state.acknowledged_turn_store.rva,
                       BytePattern::exact(acknowledged_turn));
    plan.require_bytes("Validate client network state", layout.state.network_state_guard.rva,
                       BytePattern::exact(network_state));
    plan.require_bytes("Validate melee input queue call", layout.native.input_queue_call.rva,
                       layout.native.input_queue_call.pattern());
}

} // namespace fusioncutter::patches::hero_animation_fix
