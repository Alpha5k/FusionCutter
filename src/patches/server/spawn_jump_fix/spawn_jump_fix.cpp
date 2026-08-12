#include "spawn_jump_fix.hpp"

#include "layout.hpp"

#include <algorithm>

namespace fusioncutter::patches::spawn_jump_fix {
namespace {

constexpr std::size_t kTeamCount = 8;
constexpr int kMaximumSupportedPlayers = 64;
PatchInstanceSlot<SpawnJumpFix> gPatch;

} // namespace

SpawnJumpFix::SpawnJumpFix(const TargetContext& target) noexcept
    : image_(target.image), vanish_all_players_(image_.function_at_rva<VanishAllPlayers>(layout::kVanishAllPlayersRva)),
      is_playing_(image_.function_at_rva<IsPlaying>(layout::kIsPlayingRva)),
      find_character_(image_.function_at_rva<FindCharacter>(layout::kFindCharacterRva)) {}

void SpawnJumpFix::build_plan(PatchPlan& plan) {
    plan.checked_write("End pre-game at equality", layout::kPregameEndBranchRva, layout::kStockPregameEndBranch,
                       layout::kEndPregameAtEquality);
    plan.redirect_call("Reset pre-game spawn state", layout::kVanishCallRva, BytePattern::exact(layout::kVanishCall),
                       &SpawnJumpFix::finish_pregame_hook);
}

void SpawnJumpFix::enable_runtime() noexcept {
    gPatch.publish(*this);
}

void SpawnJumpFix::disable_runtime() noexcept {
    gPatch.clear(*this);
}

// Restores each configured team delay to the runtime countdown used after warmup.
void SpawnJumpFix::reset_team_timers(std::byte* spawn_manager) noexcept {
    for (std::size_t team = 0; team < kTeamCount; ++team) {
        const auto offset = team * sizeof(float);
        const auto cycle_delay = read_native_field<float>(spawn_manager, layout::kCycleDelayOffset + offset);
        const auto slot_delay = read_native_field<float>(spawn_manager, layout::kSlotDelayOffset + offset);
        write_native_field(spawn_manager, layout::kCycleTimerOffset + offset, cycle_delay);
        write_native_field(spawn_manager, layout::kSlotTimerOffset + offset, slot_delay);
    }
}

// Requires active characters to wait for the wave following the preserved current team wave.
void SpawnJumpFix::queue_active_players(std::byte* spawn_manager) const noexcept {
    const auto configured_players = *image_.read_at_rva<int>(layout::kMaximumPlayersRva);
    const auto player_count = std::clamp(configured_players, 0, kMaximumSupportedPlayers);

    for (int player = 0; player < player_count; ++player) {
        if (!is_playing_(player, false)) {
            continue;
        }

        auto* character = find_character_(player);
        if (character == nullptr) {
            continue;
        }

        const auto team = read_native_field<int>(character, layout::kCharacterTeamOffset);
        if (team < 0 || team >= static_cast<int>(kTeamCount)) {
            continue;
        }

        const auto team_offset = static_cast<std::size_t>(team) * sizeof(std::int32_t);
        const auto current_wave = read_native_field<std::int32_t>(spawn_manager, layout::kTeamWaveOffset + team_offset);
        write_native_field(character, layout::kCharacterRequiredWaveOffset, current_wave + 1);
    }
}

// Preserves the native vanish transition before repairing its incomplete spawn state.
void SpawnJumpFix::finish_pregame() const noexcept {
    vanish_all_players_();

    auto* spawn_manager = *image_.read_at_rva<std::byte*>(layout::kSpawnManagerRva);
    if (spawn_manager == nullptr) {
        return;
    }

    reset_team_timers(spawn_manager);
    queue_active_players(spawn_manager);
}

void __cdecl SpawnJumpFix::finish_pregame_hook() noexcept {
    if (const auto* patch = gPatch.read(); patch != nullptr) {
        patch->finish_pregame();
    }
}

} // namespace fusioncutter::patches::spawn_jump_fix
