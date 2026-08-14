#include "layout.hpp"

namespace fusioncutter::patches::hero_melee_pipeline {
namespace {

constexpr auto kUpdate =
    byte_array<0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x10, 0x45, 0x08, 0x83, 0xEC, 0x48, 0x53, 0x8B, 0xD9, 0x8B, 0x4B>();
constexpr auto kSetNetworkState =
    byte_array<0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7D, 0x08, 0x3B, 0xBE, 0x80, 0x01, 0x00, 0x00>();
constexpr auto kEnterState =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30, 0x8B, 0x55, 0x08, 0x0F, 0x57, 0xC9, 0x53, 0x56, 0x57, 0x8B>();
constexpr auto kAnimatorState =
    byte_array<0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x8B, 0xD1, 0x8A, 0x4D, 0x0C, 0x57, 0x85, 0xF6, 0x75>();
constexpr auto kInputQueueUpdate =
    byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0xF3, 0x0F, 0x10, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x8B, 0xD1>();
constexpr auto kPredictionTransition =
    byte_array<0xE8, 0xE1, 0x22, 0x00, 0x00, 0x56, 0x8B, 0xCB, 0xE8, 0x19, 0x20, 0x00, 0x00, 0xF3, 0x0F, 0x10>();

constexpr HeroMeleeLayout kSteamLayout{
    .update = {.rva = 0x00289290, .expected = kUpdate},
    .set_network_state = {.rva = 0x0028B100, .expected = kSetNetworkState},
    .enter_state = {.rva = 0x0028BC30, .expected = kEnterState},
    .animator_state = {.rva = 0x0023F990, .expected = kAnimatorState},
    .input_queue_update = {.rva = 0x0028B400, .expected = kInputQueueUpdate},
    .prediction_transition = {.rva = 0x00289C0A, .expected = kPredictionTransition},
    .input_queue_constant_rva = 0x003B1F98,
};

constexpr HeroMeleeLayout kGogLayout{
    .update = {.rva = 0x0028A320, .expected = kUpdate},
    .set_network_state = {.rva = 0x0028C190, .expected = kSetNetworkState},
    .enter_state = {.rva = 0x0028CCC0, .expected = kEnterState},
    .animator_state = {.rva = 0x00240A30, .expected = kAnimatorState},
    .input_queue_update = {.rva = 0x0028C490, .expected = kInputQueueUpdate},
    .prediction_transition = {.rva = 0x0028AC9A, .expected = kPredictionTransition},
    .input_queue_constant_rva = 0x003B2F10,
};

} // namespace

const HeroMeleeLayout& layout_for(TargetLayout target) noexcept {
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

std::array<std::byte, 16> input_queue_preimage(const ImageContext& image, const HeroMeleeLayout& layout) noexcept {
    auto bytes = layout.input_queue_update.expected;
    embed_image_address<10>(bytes, image, layout.input_queue_constant_rva);
    return bytes;
}

} // namespace fusioncutter::patches::hero_melee_pipeline
