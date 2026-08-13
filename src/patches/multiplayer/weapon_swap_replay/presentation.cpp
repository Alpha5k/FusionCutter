#include "weapon_swap.hpp"

#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::weapon_swap_replay {
namespace {

constexpr std::size_t kControllablePlayerOffset = 0x94;
constexpr std::size_t kWeaponSelectTimeOffset = 0xC4;
constexpr std::size_t kSelectChannelArgumentOffset = 0x08;
constexpr std::size_t kHudWeaponChannelOffset = 0x24;
constexpr std::uintptr_t kZeroFlagMask = 1U << 6;

[[nodiscard]] std::uintptr_t with_low_byte(std::uintptr_t value, std::uint8_t low_byte) noexcept {
    return (value & ~std::uintptr_t{0xFF}) | low_byte;
}

} // namespace

void* WeaponSwapReplayFix::resolve_hud_weapon(void* controllable, int channel, void* actual_weapon) noexcept {
    observe_lifecycle();
    if (!lifecycle_.active() || channel < 0 || channel >= kWeaponChannels) {
        return actual_weapon;
    }

    auto* soldier = soldier_from_controllable(controllable);
    const int local_player = local_player_index(soldier);
    if (local_player < 0 || local_player >= kLocalPlayers) {
        return actual_weapon;
    }

    auto& tracked = local_soldiers_[static_cast<std::size_t>(local_player)];
    if (tracked != soldier) {
        ledger_.clear_player(local_player);
        packed_ledger_.clear_player(local_player);
        tracked = soldier;
        update_presentation_activity();
        return actual_weapon;
    }

    EpochSnapshot presented;
    if (!ledger_.resolve(soldier, local_player, channel, weapon_index(soldier, actual_weapon), presented)) {
        update_presentation_activity();
        return actual_weapon;
    }
    if (!validate_snapshot(presented)) {
        ledger_.clear_slot(soldier, local_player, channel);
        update_presentation_activity();
        return actual_weapon;
    }

    return presented.node_weapons[static_cast<std::size_t>(presented.final_index)];
}

std::uint8_t WeaponSwapReplayFix::resolve_packed_selection(void* soldier, std::uint8_t packed) noexcept {
    observe_lifecycle();
    if (!lifecycle_.active()) {
        return packed;
    }

    const int local_player = tracked_local_player(soldier);
    if (local_player < 0) {
        return packed;
    }

    const int actual_channel = packed_weapon_channel(packed);
    const int actual_index = packed_weapon_index(packed);
    auto* actual_weapon = weapon_at(soldier, actual_index);
    if (actual_channel < 0 || actual_channel >= kWeaponChannels || actual_index < 0 || actual_weapon == nullptr) {
        return packed;
    }

    PackedSnapshot snapshot;
    const bool presented =
        packed_ledger_.resolve(soldier, local_player, actual_channel, actual_index, actual_weapon, snapshot);
    if (snapshot.node_mask != 0 && !validate_packed_snapshot(snapshot)) {
        packed_ledger_.clear_player(local_player);
        update_presentation_activity();
        return packed;
    }
    if (!presented) {
        update_presentation_activity();
        return packed;
    }

    return PackedSwapLedger::project_selection(packed, snapshot.projected_key);
}

void WeaponSwapReplayFix::suppress_duplicate_select(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.edi == 0 || context.ebp == 0) {
        return;
    }

    auto* weapon = reinterpret_cast<void*>(context.edi);
    const int channel =
        read_native_field<int>(reinterpret_cast<const void*>(context.ebp), kSelectChannelArgumentOffset);
    if (!select_intent_.consume_duplicate(weapon, channel)) {
        return;
    }

    // Preserve the current presentation time and take Weapon::Select's native silent branch.
    context.xmm0.f32[0] = read_native_field<float>(weapon, kWeaponSelectTimeOffset);
    context.eflags &= ~kZeroFlagMask;
}

void WeaponSwapReplayFix::project_hud_weapon(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || !patch->presentation_active_ || context.esi == 0 || context.ebx == 0) {
        return;
    }

    const int channel = read_native_field<int>(reinterpret_cast<const void*>(context.ebx), kHudWeaponChannelOffset);
    context.eax = reinterpret_cast<std::uintptr_t>(
        patch->resolve_hud_weapon(reinterpret_cast<void*>(context.esi), channel, reinterpret_cast<void*>(context.eax)));
}

void WeaponSwapReplayFix::project_render_selection(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || !patch->presentation_active_ || context.edi < kControllablePlayerOffset) {
        return;
    }
    auto* soldier = reinterpret_cast<void*>(context.edi - kControllablePlayerOffset);
    context.ecx =
        with_low_byte(context.ecx, patch->resolve_packed_selection(soldier, static_cast<std::uint8_t>(context.ecx)));
}

void WeaponSwapReplayFix::project_render_channel_selection(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || !patch->presentation_active_ || context.edi < kControllablePlayerOffset) {
        return;
    }
    auto* soldier = reinterpret_cast<void*>(context.edi - kControllablePlayerOffset);
    context.eax =
        with_low_byte(context.eax, patch->resolve_packed_selection(soldier, static_cast<std::uint8_t>(context.eax)));
}

void WeaponSwapReplayFix::project_model_selection(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || !patch->presentation_active_ || context.edi < kControllablePlayerOffset) {
        return;
    }
    auto* soldier = reinterpret_cast<void*>(context.edi - kControllablePlayerOffset);
    context.edx =
        with_low_byte(context.edx, patch->resolve_packed_selection(soldier, static_cast<std::uint8_t>(context.edx)));
}

} // namespace fusioncutter::patches::weapon_swap_replay
