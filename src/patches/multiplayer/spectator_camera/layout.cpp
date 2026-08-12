#include "layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace fusioncutter::patches::spectator_camera {
namespace {

constexpr auto kRootOwner = byte_array<0x8B, 0x86, 0xA8, 0xFD, 0xFF, 0xFF, 0x8D, 0x8E, 0xA8, 0xFD, 0xFF, 0xFF, 0x57>();
constexpr auto kRootMatrixCall = byte_array<0x8B, 0x80, 0x10, 0x01, 0x00, 0x00, 0xFF, 0xD0>();
constexpr auto kRootCapture =
    byte_array<0xF3, 0x0F, 0x6F, 0x00, 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x40, 0x10>();
constexpr auto kObjectPublicationArguments = byte_array<0x50, 0xFF, 0xB7, 0x1C, 0x02, 0x00, 0x00>();
constexpr auto kObjectRenderArguments =
    byte_array<0x8D, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x50, 0xFF, 0xB7, 0x88, 0x01, 0x00, 0x00>();
constexpr auto kObjectRenderCapture = byte_array<0x8D, 0x84, 0x24, 0x84, 0x01, 0x00, 0x00>();
constexpr auto kCameraInterpolatorOwner =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x0C, 0x01, 0x00, 0x00, 0x56, 0x8B, 0xF1>();
constexpr auto kCameraRenderArguments =
    byte_array<0x8D, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00, 0x8B, 0x8E, 0xA0, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x84,
               0x24, 0xE0, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x08, 0xF3, 0x0F, 0x11, 0x84, 0x24, 0xE4,
               0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x0C, 0x50>();

constexpr auto kSteamObjectPublicationCall = byte_array<0xE8, 0xB4, 0x05, 0x05, 0x00>();
constexpr auto kGogObjectPublicationCall = byte_array<0xE8, 0x24, 0x13, 0x05, 0x00>();
constexpr auto kSteamObjectRenderCall = byte_array<0xE8, 0x54, 0x4B, 0x05, 0x00>();
constexpr auto kGogObjectRenderCall = byte_array<0xE8, 0xC4, 0x58, 0x05, 0x00>();
constexpr auto kCameraPublicationCall = byte_array<0xE8, 0x34, 0x05, 0x00, 0x00>();
constexpr auto kSteamCameraRenderCall = byte_array<0xE8, 0x60, 0xC6, 0x27, 0x00>();
constexpr auto kGogCameraRenderCall = byte_array<0xE8, 0x20, 0xD7, 0x27, 0x00>();

constexpr SpectatorCameraLayout kSteamLayout{
    .hooks =
        {
            .root_capture = {.rva = 0x000F22B4, .expected = kRootCapture},
            .object_publication = {.rva = 0x000E6E47, .expected = kSteamObjectPublicationCall},
            .object_render_call = {.rva = 0x000E2907, .expected = kSteamObjectRenderCall},
            .object_render_capture = {.rva = 0x000E290C, .expected = kObjectRenderCapture},
            .camera_publication = {.rva = 0x0004F377, .expected = kCameraPublicationCall},
            .camera_render = {.rva = 0x0004F88B, .expected = kSteamCameraRenderCall},
        },
    .contexts =
        {
            .root_owner = {.rva = 0x000F229F, .expected = kRootOwner},
            .root_matrix_call = {.rva = 0x000F22AC, .expected = kRootMatrixCall},
            .object_publication_arguments = {.rva = 0x000E6E3B, .expected = kObjectPublicationArguments},
            .object_render_arguments = {.rva = 0x000E28F9, .expected = kObjectRenderArguments},
            .camera_publication_arguments =
                {.rva = 0x0004F360,
                 .expected = byte_array<0x8B, 0x46, 0x50, 0x69, 0xC8, 0xB0, 0x00, 0x00, 0x00, 0xFF, 0x74, 0x86, 0x24,
                                        0xFF, 0x74, 0x86, 0x20, 0x81, 0xC1, 0xD0, 0x0F, 0xEB, 0x01>()},
            .camera_interpolator_owner = {.rva = 0x0004F6E0, .expected = kCameraInterpolatorOwner},
            .camera_render_arguments = {.rva = 0x0004F84B, .expected = kCameraRenderArguments},
        },
    .state =
        {
            .mode_probe_rva = 0x000F23C9,
            .state_probe_rva = 0x000F23F8,
            .update_turn_access_rva = 0x001BA998,
            .turn_ratio_getter_rva = 0x001BA740,
            .update_turn_rva = 0x01BA65B0,
            .turn_ratio_rva = 0x01BA87B8,
            .network_provider_flag_rva = 0x003E8007,
            .spectator_enabled_rva = 0x01A62EA9,
            .alternate_spectator_enabled_rva = 0x01A62EA8,
            .net_game_global_rva = 0x01A30324,
            .get_local_index_rva = 0x001B73B0,
            .get_spectator_rva = 0x001C3950,
        },
};

constexpr SpectatorCameraLayout kGogLayout{
    .hooks =
        {
            .root_capture = {.rva = 0x000F22B4, .expected = kRootCapture},
            .object_publication = {.rva = 0x000E6E47, .expected = kGogObjectPublicationCall},
            .object_render_call = {.rva = 0x000E2907, .expected = kGogObjectRenderCall},
            .object_render_capture = {.rva = 0x000E290C, .expected = kObjectRenderCapture},
            .camera_publication = {.rva = 0x0004F357, .expected = kCameraPublicationCall},
            .camera_render = {.rva = 0x0004F86B, .expected = kGogCameraRenderCall},
        },
    .contexts =
        {
            .root_owner = {.rva = 0x000F229F, .expected = kRootOwner},
            .root_matrix_call = {.rva = 0x000F22AC, .expected = kRootMatrixCall},
            .object_publication_arguments = {.rva = 0x000E6E3B, .expected = kObjectPublicationArguments},
            .object_render_arguments = {.rva = 0x000E28F9, .expected = kObjectRenderArguments},
            .camera_publication_arguments =
                {.rva = 0x0004F340,
                 .expected = byte_array<0x8B, 0x46, 0x50, 0x69, 0xC8, 0xB0, 0x00, 0x00, 0x00, 0xFF, 0x74, 0x86, 0x24,
                                        0xFF, 0x74, 0x86, 0x20, 0x81, 0xC1, 0x80, 0x24, 0xEB, 0x01>()},
            .camera_interpolator_owner = {.rva = 0x0004F6C0, .expected = kCameraInterpolatorOwner},
            .camera_render_arguments = {.rva = 0x0004F82B, .expected = kCameraRenderArguments},
        },
    .state =
        {
            .mode_probe_rva = 0x000F23C9,
            .state_probe_rva = 0x000F23F8,
            .update_turn_access_rva = 0x001BB948,
            .turn_ratio_getter_rva = 0x001BB6F0,
            .update_turn_rva = 0x01BA7A64,
            .turn_ratio_rva = 0x01BA9C6C,
            .network_provider_flag_rva = 0x003E9007,
            .spectator_enabled_rva = 0x01A64359,
            .alternate_spectator_enabled_rva = 0x01A64358,
            .net_game_global_rva = 0x01A317C4,
            .get_local_index_rva = 0x001B8360,
            .get_spectator_rva = 0x001C48E0,
        },
};

} // namespace

const SpectatorCameraLayout& layout_for(TargetLayout target) noexcept {
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

void add_layout_requirements(PatchPlan& plan, const ImageContext& image, const SpectatorCameraLayout& layout) {
    plan.require_bytes("Validate spectator root owner", layout.contexts.root_owner.rva,
                       layout.contexts.root_owner.pattern());
    plan.require_bytes("Validate spectator root matrix", layout.contexts.root_matrix_call.rva,
                       layout.contexts.root_matrix_call.pattern());
    plan.require_bytes("Validate object publication arguments", layout.contexts.object_publication_arguments.rva,
                       layout.contexts.object_publication_arguments.pattern());
    plan.require_bytes("Validate object render arguments", layout.contexts.object_render_arguments.rva,
                       layout.contexts.object_render_arguments.pattern());
    plan.require_bytes("Validate native object interpolation", layout.hooks.object_render_call.rva,
                       layout.hooks.object_render_call.pattern());
    plan.require_bytes("Validate camera publication arguments", layout.contexts.camera_publication_arguments.rva,
                       layout.contexts.camera_publication_arguments.pattern());
    plan.require_bytes("Validate camera interpolator owner", layout.contexts.camera_interpolator_owner.rva,
                       layout.contexts.camera_interpolator_owner.pattern());
    plan.require_bytes("Validate camera render arguments", layout.contexts.camera_render_arguments.rva,
                       layout.contexts.camera_render_arguments.pattern());

    auto mode_probe =
        byte_array<0x38, 0x15, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x0D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x7E, 0x44,
                   0x24, 0x24, 0x89, 0x44, 0x24, 0x38, 0x0F, 0xB6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x45, 0xC8>();
    embed_image_address<2>(mode_probe, image, layout.state.network_provider_flag_rva);
    embed_image_address<9>(mode_probe, image, layout.state.spectator_enabled_rva);
    embed_image_address<26>(mode_probe, image, layout.state.alternate_spectator_enabled_rva);
    plan.require_bytes("Validate spectator mode selection", layout.state.mode_probe_rva,
                       BytePattern::exact(mode_probe));

    auto state_probe = byte_array<0xA1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x70, 0x50, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x83,
                                  0xC4, 0x04, 0x50, 0xE8, 0x00, 0x00, 0x00, 0x00>();
    embed_image_address<1>(state_probe, image, layout.state.net_game_global_rva);
    embed_relative_displacement<9>(state_probe, layout.state.state_probe_rva + 13, layout.state.get_local_index_rva);
    embed_relative_displacement<18>(state_probe, layout.state.state_probe_rva + 22, layout.state.get_spectator_rva);
    plan.require_bytes("Validate active spectator lookup", layout.state.state_probe_rva,
                       BytePattern::exact(state_probe));

    auto update_turn = byte_array<0x3B, 0x15, 0x00, 0x00, 0x00, 0x00>();
    embed_image_address<2>(update_turn, image, layout.state.update_turn_rva);
    plan.require_bytes("Validate network update turn", layout.state.update_turn_access_rva,
                       BytePattern::exact(update_turn));

    auto turn_ratio = byte_array<0x55, 0x8B, 0xEC, 0xD9, 0x05, 0x00, 0x00, 0x00, 0x00, 0x5D, 0xC3>();
    embed_image_address<5>(turn_ratio, image, layout.state.turn_ratio_rva);
    plan.require_bytes("Validate interpolation turn ratio", layout.state.turn_ratio_getter_rva,
                       BytePattern::exact(turn_ratio));
}

} // namespace fusioncutter::patches::spectator_camera
