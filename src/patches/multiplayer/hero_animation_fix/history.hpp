#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::hero_animation_fix {

inline constexpr std::size_t kLocalPlayers = 2;
inline constexpr std::size_t kNetworkPlayers = 64;
inline constexpr std::size_t kRemoteStateCapacity = 32;

// Binds reconciliation state to one exact melee weapon and its owning player lifecycle.
struct HeroIdentity {
    std::uintptr_t weapon{};
    std::uintptr_t owner{};
    std::uintptr_t combo{};
    std::int16_t player_handle{-1};
    std::uint8_t local_player{0xFF};

    [[nodiscard]] constexpr bool is_local() const noexcept {
        return static_cast<std::size_t>(local_player) < kLocalPlayers;
    }

    [[nodiscard]] constexpr bool is_remote() const noexcept {
        return local_player == 0xFF && player_handle >= 0 && static_cast<std::size_t>(player_handle) < kNetworkPlayers;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return weapon != 0 && owner != 0 && combo != 0 && player_handle >= 0 &&
               static_cast<std::size_t>(player_handle) < kNetworkPlayers && (is_local() || is_remote());
    }

    friend constexpr bool operator==(const HeroIdentity&, const HeroIdentity&) = default;
};

enum class AuthorityAction {
    Apply,
    SuppressHistorical,
};

// Reconciles local acknowledged actions, ordered remote states, and authority-first transition receipts.
class HeroHistory {
  public:
    [[nodiscard]] bool observe_prediction(const HeroIdentity& identity, int base_state, int target_state,
                                          std::int32_t prediction_turn) noexcept;
    void begin_prediction(const HeroIdentity& identity) noexcept;
    [[nodiscard]] AuthorityAction classify_authority(const HeroIdentity& identity, int current_state,
                                                     int incoming_state, std::int32_t acknowledged_turn) noexcept;

    void record_authority_transition(const HeroIdentity& identity, int state) noexcept;
    [[nodiscard]] bool reconcile_input(const HeroIdentity& identity, std::uint8_t buttons,
                                       std::uint8_t& down_mask) noexcept;
    [[nodiscard]] bool resolve_replay(const HeroIdentity& identity, int target_state) noexcept;
    void finish_prediction(const HeroIdentity& identity) noexcept;

    [[nodiscard]] bool local_action_active(const HeroIdentity& identity) const noexcept;
    [[nodiscard]] bool should_suppress_local_presentation(const HeroIdentity& identity, int current_state,
                                                          std::int32_t acknowledged_turn) const noexcept;

    void clear(const HeroIdentity& identity) noexcept;
    void clear_weapon(std::uintptr_t weapon) noexcept;
    void clear_all() noexcept;

  private:
    struct LocalEpoch {
        HeroIdentity identity{};
        std::uint32_t seen_states{};
        std::int32_t start_turn{-1};
        std::int32_t finish_turn{-1};
        std::int8_t predicted_state{-1};
    };

    struct RemoteSlot {
        HeroIdentity identity{};
        std::array<std::int8_t, kRemoteStateCapacity> pending{};
        std::uint8_t count{};
        std::int8_t authority_state{-1};
        bool authority_seen{};
    };

    struct AuthorityReceipt {
        HeroIdentity identity{};
        std::int8_t replay_state{-1};
        bool reconcile_input{};
    };

    [[nodiscard]] bool observe_local_prediction(const HeroIdentity& identity, int base_state, int target_state,
                                                std::int32_t prediction_turn) noexcept;
    [[nodiscard]] bool observe_remote_prediction(const HeroIdentity& identity, int base_state,
                                                 int target_state) noexcept;
    [[nodiscard]] AuthorityAction classify_local_authority(const HeroIdentity& identity, int current_state,
                                                           int incoming_state, std::int32_t acknowledged_turn) noexcept;
    [[nodiscard]] AuthorityAction classify_remote_authority(const HeroIdentity& identity, int current_state,
                                                            int incoming_state) noexcept;

    [[nodiscard]] RemoteSlot& bind_remote(const HeroIdentity& identity) noexcept;
    [[nodiscard]] AuthorityReceipt& bind_receipt(const HeroIdentity& identity) noexcept;
    static void seed_remote(RemoteSlot& slot, int state) noexcept;
    [[nodiscard]] static int expected_remote_state(const RemoteSlot& slot) noexcept;
    [[nodiscard]] static std::size_t find_pending(const RemoteSlot& slot, int state) noexcept;
    static void consume_pending(RemoteSlot& slot, std::size_t count) noexcept;

    std::array<LocalEpoch, kLocalPlayers> local_{};
    std::array<RemoteSlot, kNetworkPlayers> remote_{};
    std::array<AuthorityReceipt, kNetworkPlayers> receipts_{};
};

} // namespace fusioncutter::patches::hero_animation_fix
