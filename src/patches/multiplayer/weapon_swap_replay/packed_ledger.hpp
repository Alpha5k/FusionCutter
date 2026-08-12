#pragma once

#include "local_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::weapon_swap_replay {

struct PackedDecision {
    bool mute_select{};
    bool pointer_invalidated{};
};

struct PackedSnapshot {
    void* soldier{};
    std::array<void*, kWeaponChannels * kWeaponIndices> node_weapons{};
    std::uint16_t node_mask{};
    std::uint8_t projected_key{};
};

// Keeps the player-wide packed selection alive when one channel settles before the other.
class PackedSwapLedger {
  public:
    static constexpr int kKeys = kWeaponChannels * kWeaponIndices;
    static constexpr std::int32_t kReplayHistoryTurns = LocalSwapLedger::kReplayHistoryTurns;

    // Expire packed ownership at the same native input-history boundary as channel requests.
    void observe_frontiers(const TurnFrontiers& frontiers) noexcept;
    // Bind the player-wide packed lane to the latest projected channel request.
    [[nodiscard]] bool track_projected(void* soldier, int local_player, int channel, int index, void* weapon,
                                       std::int32_t request_turn, std::uint32_t epoch, std::uint32_t sequence,
                                       const TurnFrontiers& frontiers) noexcept;
    // Extend packed ownership across a verified native rollback transition.
    [[nodiscard]] PackedDecision observe_transition(void* soldier, int local_player, int old_channel, int old_index,
                                                    void* old_weapon, int target_channel, int target_index,
                                                    void* target_weapon, const TurnFrontiers& frontiers) noexcept;
    [[nodiscard]] PackedDecision observe_sync(void* soldier, int local_player, int channel, int index, void* weapon,
                                              const TurnFrontiers& frontiers) noexcept;
    // Retain accepted packed ownership until the player-wide authoritative value catches up.
    void observe_channel_result(int local_player, const AuthoritativeDecision& decision) noexcept;
    // Project a known packed endpoint while preserving native availability and fallback checks.
    [[nodiscard]] bool resolve(void* soldier, int local_player, int actual_channel, int actual_index,
                               void* actual_weapon, PackedSnapshot& snapshot) noexcept;

    [[nodiscard]] bool has_lane(void* soldier, int local_player) const noexcept;
    [[nodiscard]] bool find(void* soldier, int local_player, PackedSnapshot& snapshot) const noexcept;
    void clear_player(int local_player) noexcept;
    void clear_epoch(void* soldier, int local_player, std::uint32_t epoch) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool has_active() const noexcept;

    [[nodiscard]] static constexpr int key(int channel, int index) noexcept {
        return channel * kWeaponIndices + index;
    }

    [[nodiscard]] static constexpr std::uint8_t project_selection(std::uint8_t packed, int projected_key) noexcept {
        if (projected_key < 0 || projected_key >= kKeys) {
            return packed;
        }
        return static_cast<std::uint8_t>((packed & 0xC0U) |
                                         (static_cast<std::uint8_t>(projected_key / kWeaponIndices) << 4) |
                                         static_cast<std::uint8_t>(projected_key % kWeaponIndices));
    }

  private:
    struct Lane {
        void* soldier{};
        std::array<void*, kKeys> node_weapons{};
        std::uint16_t node_mask{};
        std::uint8_t projected_key{};
        std::int32_t latest_request_turn{-1};
        std::int32_t high_predict{-1};
        std::uint32_t epoch{};
        std::uint32_t sequence{};
        bool active{};
        bool settled{};
    };

    [[nodiscard]] static bool valid_owner(void* soldier, int local_player) noexcept;
    [[nodiscard]] static bool valid_key(int channel, int index) noexcept;
    static void advance(Lane& lane, const TurnFrontiers& frontiers) noexcept;
    static void start(Lane& lane, void* soldier, int projected_key, void* weapon, std::int32_t request_turn,
                      std::uint32_t epoch, std::uint32_t sequence, const TurnFrontiers& frontiers) noexcept;
    [[nodiscard]] static PackedSnapshot make_snapshot(const Lane& lane) noexcept;

    std::array<Lane, kLocalPlayers> lanes_{};
};

} // namespace fusioncutter::patches::weapon_swap_replay
