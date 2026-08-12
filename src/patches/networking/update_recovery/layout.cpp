#include "layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace fusioncutter::patches::update_recovery {
namespace {

constexpr auto kDrainPreimage = byte_array<0xBA, 0x01, 0x00, 0x00, 0x00, 0x85, 0xD2, 0x0F, 0x84, 0x17, 0x01, 0x00, 0x00,
                                           0x68, 0x00, 0x00, 0x00, 0x00>();
constexpr auto kConstructGroupPreimage = byte_array<0xC6, 0x01, 0x00, 0x8B, 0xC1, 0xC3>();
constexpr auto kReceiveGroupPreimage = byte_array<0x56, 0x8B, 0xF2, 0xBA, 0x1E, 0x00, 0x00, 0x00>();
constexpr auto kGetUpdateTurnPreimage = byte_array<0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x4D, 0x08>();
constexpr auto kReceiveUpdatePreimage = byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08>();
constexpr auto kGetLastReceivePreimage =
    byte_array<0x8D, 0x04, 0x49, 0xC1, 0xE0, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00, 0xC3>();
constexpr auto kStaleGuardPreimage = byte_array<0x3B, 0x15, 0x00, 0x00, 0x00, 0x00, 0x77, 0x05, 0xE9, 0xD0, 0x06, 0x00,
                                                0x00, 0x8B, 0x45, 0xE4, 0xA3, 0x00, 0x00, 0x00, 0x00>();

constexpr auto kSteamPipeInputPreimage = byte_array<0x33, 0xC9, 0xE8, 0xE0, 0x96, 0xFF, 0xFF, 0x69, 0xD0, 0x08, 0x02,
                                                    0x00, 0x00, 0x8B, 0x82, 0x00, 0x00, 0x00, 0x00, 0x89, 0x45, 0xEC>();
constexpr auto kSteamTimestampPreimage = byte_array<0xE8, 0x7A, 0x98, 0xFF, 0xFF, 0xF3, 0x0F, 0x11, 0x45, 0xF0>();
constexpr auto kSteamResumePreimage =
    byte_array<0xC6, 0x45, 0xFE, 0x37, 0x6A, 0x00, 0x8D, 0x4D, 0xFE, 0x51, 0xE8, 0xF2, 0xF7, 0xE9, 0xFF>();
constexpr auto kSteamTimerRefreshPreimage =
    byte_array<0x8B, 0x4D, 0xEC, 0x83, 0xC1, 0x40, 0xE8, 0x64, 0x9B, 0xFF, 0xFF, 0xF3, 0x0F, 0x10, 0x45, 0xF0, 0xF3,
               0x0F, 0x11, 0x00, 0xF3, 0x0F, 0x10, 0x45, 0xF0, 0xF3, 0x0F, 0x58, 0x05, 0x00, 0x00, 0x00, 0x00, 0xF3,
               0x0F, 0x11, 0x05, 0x00, 0x00, 0x00, 0x00>();

constexpr auto kGogPipeInputPreimage = byte_array<0x33, 0xC9, 0xE8, 0xD0, 0x96, 0xFF, 0xFF, 0x69, 0xD0, 0x08, 0x02,
                                                  0x00, 0x00, 0x8B, 0x82, 0x00, 0x00, 0x00, 0x00, 0x89, 0x45, 0xEC>();
constexpr auto kGogTimestampPreimage = byte_array<0xE8, 0x6A, 0x98, 0xFF, 0xFF, 0xF3, 0x0F, 0x11, 0x45, 0xF0>();
constexpr auto kGogResumePreimage =
    byte_array<0xC6, 0x45, 0xFE, 0x37, 0x6A, 0x00, 0x8D, 0x4D, 0xFE, 0x51, 0xE8, 0x22, 0xE8, 0xE9, 0xFF>();
constexpr auto kGogTimerRefreshPreimage =
    byte_array<0x8B, 0x4D, 0xEC, 0x83, 0xC1, 0x40, 0xE8, 0x54, 0x9B, 0xFF, 0xFF, 0xF3, 0x0F, 0x10, 0x45, 0xF0, 0xF3,
               0x0F, 0x11, 0x00, 0xF3, 0x0F, 0x10, 0x45, 0xF0, 0xF3, 0x0F, 0x58, 0x05, 0x00, 0x00, 0x00, 0x00, 0xF3,
               0x0F, 0x11, 0x05, 0x00, 0x00, 0x00, 0x00>();

constexpr UpdateRecoveryLayout kSteamLayout{
    .frame =
        {
            .pipe_input = {.rva = 0x001B8FF9, .expected = kSteamPipeInputPreimage},
            .pipe_table_rva = 0x01ACDBCC,
            .timestamp = {.rva = 0x001B9021, .expected = kSteamTimestampPreimage},
            .drain = {.rva = 0x001B902B, .expected = kDrainPreimage},
            .resume = {.rva = 0x001B914F, .expected = kSteamResumePreimage},
        },
    .functions =
        {
            .construct_group = {.rva = 0x001AECA0, .expected = kConstructGroupPreimage},
            .receive_group = {.rva = 0x001B2A60, .expected = kReceiveGroupPreimage},
            .get_update_turn = {.rva = 0x001BB570, .expected = kGetUpdateTurnPreimage},
            .receive_update = {.rva = 0x001BB530, .expected = kReceiveUpdatePreimage},
            .get_last_receive = {.rva = 0x001B2C90, .expected = kGetLastReceivePreimage},
            .last_receive_table_rva = 0x01ACC0FC,
        },
    .state =
        {
            .stale_guard = {.rva = 0x001BB66A, .expected = kStaleGuardPreimage},
            .client_host_turn_rva = 0x01A64DE4,
            .timer_refresh = {.rva = 0x001B9121, .expected = kSteamTimerRefreshPreimage},
            .host_cycle_extension_rva = 0x003B221C,
            .host_cycle_timer_rva = 0x01BA87A0,
        },
};

constexpr UpdateRecoveryLayout kGogLayout{
    .frame =
        {
            .pipe_input = {.rva = 0x001B9FA9, .expected = kGogPipeInputPreimage},
            .pipe_table_rva = 0x01ACF07C,
            .timestamp = {.rva = 0x001B9FD1, .expected = kGogTimestampPreimage},
            .drain = {.rva = 0x001B9FDB, .expected = kDrainPreimage},
            .resume = {.rva = 0x001BA0FF, .expected = kGogResumePreimage},
        },
    .functions =
        {
            .construct_group = {.rva = 0x001AFC50, .expected = kConstructGroupPreimage},
            .receive_group = {.rva = 0x001B3A00, .expected = kReceiveGroupPreimage},
            .get_update_turn = {.rva = 0x001BC520, .expected = kGetUpdateTurnPreimage},
            .receive_update = {.rva = 0x001BC4E0, .expected = kReceiveUpdatePreimage},
            .get_last_receive = {.rva = 0x001B3C30, .expected = kGetLastReceivePreimage},
            .last_receive_table_rva = 0x01ACD5AC,
        },
    .state =
        {
            .stale_guard = {.rva = 0x001BC61A, .expected = kStaleGuardPreimage},
            .client_host_turn_rva = 0x01A66294,
            .timer_refresh = {.rva = 0x001BA0D1, .expected = kGogTimerRefreshPreimage},
            .host_cycle_extension_rva = 0x003B3194,
            .host_cycle_timer_rva = 0x01BA9C54,
        },
};

} // namespace

const UpdateRecoveryLayout& layout_for(TargetLayout target) noexcept {
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

void add_layout_requirements(PatchPlan& plan, const ImageContext& image, const UpdateRecoveryLayout& layout) {
    auto pipe_input = layout.frame.pipe_input.expected;
    embed_image_address<15>(pipe_input, image, layout.frame.pipe_table_rva);
    auto get_last_receive = layout.functions.get_last_receive.expected;
    embed_image_address<7>(get_last_receive, image, layout.functions.last_receive_table_rva);
    auto stale_guard = layout.state.stale_guard.expected;
    embed_image_address<2>(stale_guard, image, layout.state.client_host_turn_rva);
    embed_image_address<17>(stale_guard, image, layout.state.client_host_turn_rva);
    auto timer_refresh = layout.state.timer_refresh.expected;
    embed_image_address<29>(timer_refresh, image, layout.state.host_cycle_extension_rva);
    embed_image_address<37>(timer_refresh, image, layout.state.host_cycle_timer_rva);

    plan.require_bytes("Validate update receive pipe input", layout.frame.pipe_input.rva,
                       BytePattern::exact(pipe_input));
    plan.require_bytes("Validate update receive timestamp", layout.frame.timestamp.rva,
                       layout.frame.timestamp.pattern());
    plan.require_bytes("Validate update drain resume", layout.frame.resume.rva, layout.frame.resume.pattern());
    plan.require_bytes("Validate native packet-group construction", layout.functions.construct_group.rva,
                       layout.functions.construct_group.pattern());
    plan.require_bytes("Validate native packet-group receive", layout.functions.receive_group.rva,
                       layout.functions.receive_group.pattern());
    plan.require_bytes("Validate native update-turn reader", layout.functions.get_update_turn.rva,
                       layout.functions.get_update_turn.pattern());
    plan.require_bytes("Validate native update receiver", layout.functions.receive_update.rva,
                       layout.functions.receive_update.pattern());
    plan.require_bytes("Validate native last-receive lookup", layout.functions.get_last_receive.rva,
                       BytePattern::exact(get_last_receive));
    plan.require_bytes("Validate native stale-update guard", layout.state.stale_guard.rva,
                       BytePattern::exact(stale_guard));
    plan.require_bytes("Validate native receive-timer update", layout.state.timer_refresh.rva,
                       BytePattern::exact(timer_refresh));
}

std::array<std::byte, 18> drain_preimage(const ImageContext& image, const UpdateRecoveryLayout& layout) noexcept {
    auto bytes = layout.frame.drain.expected;
    embed_image_address<14>(bytes, image, layout.functions.construct_group.rva);
    return bytes;
}

} // namespace fusioncutter::patches::update_recovery
