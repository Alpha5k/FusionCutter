#pragma once

#include "state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::weapon_swap_replay {

// Retains one causal request chain for each local player and weapon channel.
class LocalSwapLedger {
  public:
    static constexpr std::size_t kMaxSteps = 32;
    static constexpr std::int32_t kSettlementTurns = 2;
    static constexpr std::int32_t kReplayHistoryTurns = 32;

    // Advance high-water marks and expire requests beyond the native input-history window.
    void observe_frontiers(const TurnFrontiers& frontiers) noexcept;
    // Add a local transition or recognize it as historical prediction replay.
    [[nodiscard]] RequestDecision record_local(void* soldier, int local_player, int channel, int old_index,
                                               int target_index, void* old_weapon, void* target_weapon,
                                               std::int32_t request_turn, const TurnFrontiers& frontiers) noexcept;
    // Settle a request only after update and acknowledgement frontiers pass its replay window.
    [[nodiscard]] AuthoritativeDecision observe_authoritative(void* soldier, int local_player, int channel,
                                                              int current_index, int server_index, void* current_weapon,
                                                              void* server_weapon,
                                                              const TurnFrontiers& frontiers) noexcept;

    [[nodiscard]] bool classify_node_selection(void* soldier, int local_player, int channel, int target_index,
                                               void* target_weapon) const noexcept;
    // Project an earlier known node to the latest local endpoint for presentation only.
    [[nodiscard]] bool resolve(void* soldier, int local_player, int channel, int actual_index,
                               EpochSnapshot& snapshot) noexcept;
    [[nodiscard]] bool find(void* soldier, int local_player, int channel, EpochSnapshot& snapshot) const noexcept;

    void clear_slot(void* soldier, int local_player, int channel) noexcept;
    void clear_player(int local_player) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool has_active() const noexcept;

  private:
    struct Step {
        std::int32_t request_turn{-1};
    };

    struct Epoch {
        void* soldier{};
        std::array<void*, kWeaponIndices> node_weapons{};
        std::array<Step, kMaxSteps> steps{};
        std::size_t step_count{};
        std::uint8_t node_mask{};
        std::uint8_t final_index{};
        std::int32_t latest_request_turn{-1};
        std::int32_t high_update{-1};
        std::int32_t high_predict{-1};
        std::int32_t high_acknowledged{-1};
        std::int32_t settlement_start_update_turn{-1};
        std::uint32_t id{};
        std::uint32_t latest_sequence{};
        bool active{};
    };

    [[nodiscard]] static bool valid_owner(void* soldier, int local_player, int channel) noexcept;
    [[nodiscard]] static bool valid_index(int index) noexcept;
    static void advance(Epoch& epoch, const TurnFrontiers& frontiers) noexcept;
    [[nodiscard]] static std::uint32_t next_nonzero(std::uint32_t& value) noexcept;
    [[nodiscard]] static bool matches_node(const Epoch& epoch, int index, void* weapon) noexcept;
    [[nodiscard]] static RequestDecision abandon(Epoch& epoch, RequestKind kind) noexcept;
    [[nodiscard]] RequestDecision start(Epoch& epoch, void* soldier, int old_index, int target_index, void* old_weapon,
                                        void* target_weapon, std::int32_t request_turn, const TurnFrontiers& frontiers,
                                        RequestKind kind) noexcept;
    [[nodiscard]] static EpochSnapshot make_snapshot(const Epoch& epoch) noexcept;
    [[nodiscard]] static std::size_t slot(int local_player, int channel) noexcept;
    [[nodiscard]] Epoch& epoch_at(int local_player, int channel) noexcept;
    [[nodiscard]] const Epoch& epoch_at(int local_player, int channel) const noexcept;

    std::array<Epoch, kLocalPlayers * kWeaponChannels> epochs_{};
    std::uint32_t next_epoch_{};
    std::uint32_t next_sequence_{};
};

} // namespace fusioncutter::patches::weapon_swap_replay
