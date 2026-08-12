#include "weapon_swap.hpp"

#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::weapon_swap_replay {
namespace {

constexpr std::size_t kMoveButtonsOffset = 0x10;
constexpr std::size_t kSwitchTriggerOffset = 0x78;
constexpr std::size_t kReturnAddressOffset = 0x04;
constexpr std::size_t kMoveSize = 0x18;
constexpr std::size_t kMoveTurnSize = 0x30;
constexpr int kMoveHistoryTurns = 32;

} // namespace

bool WeaponSwapReplayFix::held_switch_replay(int local_player, int channel, std::uint32_t trigger,
                                             std::uint32_t direction) const noexcept {
    if (local_player < 0 || local_player >= kLocalPlayers) {
        return false;
    }

    const auto predict_turn = *predict_turn_;
    const auto current_age = static_cast<std::int64_t>(*client_turn_) - predict_turn;
    if (predict_turn <= 0 || current_age < 0 || current_age + 1 >= kMoveHistoryTurns) {
        return false;
    }

    const auto read_buttons = [this, local_player](int turn) noexcept {
        const auto offset = static_cast<std::size_t>(turn & (kMoveHistoryTurns - 1)) * kMoveTurnSize +
                            static_cast<std::size_t>(local_player) * kMoveSize + kMoveButtonsOffset;
        return read_native_field<std::uint32_t>(local_move_history_, offset);
    };
    return held_switch_edge(channel, trigger, direction, read_buttons(predict_turn - 1), read_buttons(predict_turn));
}

bool WeaponSwapReplayFix::suppress_held_switch(void* controllable, int channel,
                                               std::uint32_t direction) const noexcept {
    if (controllable == nullptr || channel < 0 || channel >= kWeaponChannels || !network_prediction_active()) {
        return false;
    }

    auto* soldier = soldier_from_controllable(controllable);
    const int local_player = local_player_index(soldier);
    const auto trigger = read_native_field<std::uint32_t>(
        controllable, kSwitchTriggerOffset + static_cast<std::size_t>(channel) * sizeof(std::uint32_t));
    return held_switch_replay(local_player, channel, trigger, direction);
}

void WeaponSwapReplayFix::filter_held_switch(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.ecx == 0 || context.ebp == 0) {
        return;
    }

    const auto return_address =
        read_native_field<std::uintptr_t>(reinterpret_cast<const void*>(context.ebp), kReturnAddressOffset);
    if (return_address != patch->switch_primary_return_ && return_address != patch->switch_secondary_return_) {
        return;
    }

    if (patch->suppress_held_switch(reinterpret_cast<void*>(context.ecx), static_cast<int>(context.edx),
                                    context.xmm1.u32[0])) {
        // The native SetSwitch store immediately consumes XMM1 after this callback.
        context.xmm1.u32[0] = 0;
    }
}

} // namespace fusioncutter::patches::weapon_swap_replay
