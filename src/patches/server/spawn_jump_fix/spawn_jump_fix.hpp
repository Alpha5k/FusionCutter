#pragma once

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::spawn_jump_fix {

// Re-arms team timers and active-player wave requirements when pre-game ends.
class SpawnJumpFix final : public RuntimePatch {
  public:
    explicit SpawnJumpFix(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using VanishAllPlayers = void(__cdecl*)();
    using IsPlaying = bool(__cdecl*)(int, bool);
    using FindCharacter = std::byte*(__fastcall*)(int);

    ImageContext image_;
    VanishAllPlayers vanish_all_players_;
    IsPlaying is_playing_;
    FindCharacter find_character_;

    void finish_pregame() const noexcept;
    static void reset_team_timers(std::byte* spawn_manager) noexcept;
    void queue_active_players(std::byte* spawn_manager) const noexcept;
    static void __cdecl finish_pregame_hook() noexcept;
};

} // namespace fusioncutter::patches::spawn_jump_fix
