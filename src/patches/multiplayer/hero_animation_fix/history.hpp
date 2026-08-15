#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::hero_animation_fix {

inline constexpr std::size_t kLocalPlayers = 2;
inline constexpr std::size_t kNetworkPlayers = 64;
inline constexpr std::size_t kTransitionCapacity = 32;

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
    Native,
    Historical,
    Corrected,
};

// Reconciles local authority against the ordered state path at its acknowledged prediction turn.
class LocalHistory {
  public:
    // Records each predicted transition occurrence at the turn that produced it.
    [[nodiscard]] bool observe_prediction(const HeroIdentity& identity, int base_state, int target_state,
                                          std::int32_t prediction_turn) noexcept;
    // Compares authority with the predicted state at the acknowledged turn.
    [[nodiscard]] AuthorityAction classify_authority(const HeroIdentity& identity, int current_state,
                                                     int incoming_state, std::int32_t acknowledged_turn) noexcept;

    void clear(const HeroIdentity& identity) noexcept;
    void clear_weapon(std::uintptr_t weapon) noexcept;
    void clear_all() noexcept;

  private:
    struct TransitionOccurrence {
        std::int32_t turn{-1};
        std::int8_t to_state{-1};
    };

    struct Slot {
        HeroIdentity identity{};
        std::array<TransitionOccurrence, kTransitionCapacity> path{};
        std::uint8_t count{};
        std::int8_t base_state{-1};
        std::int8_t predicted_state{-1};
        std::int32_t base_turn{-1};
    };

    [[nodiscard]] Slot& bind(const HeroIdentity& identity) noexcept;
    static void seed(Slot& slot, const HeroIdentity& identity, int state, std::int32_t turn) noexcept;
    static void advance(Slot& slot, std::size_t count, int state, std::int32_t turn) noexcept;

    std::array<Slot, kLocalPlayers> slots_{};
};

// Reconciles remote public states and the private held-input history omitted by the network stream.
class RemoteHistory {
  public:
    // Records the ordered public-state path already presented for a remote hero.
    [[nodiscard]] bool observe_prediction(const HeroIdentity& identity, int base_state, int target_state,
                                          std::int32_t update_turn) noexcept;
    // Advances the remote authority frontier while allowing one distinct newer idle observation.
    [[nodiscard]] AuthorityAction classify_authority(const HeroIdentity& identity, int current_state,
                                                     int incoming_state, std::int32_t update_turn) noexcept;

    void begin_authority(const HeroIdentity& identity) noexcept;
    // Marks a non-idle authority transition that prediction must not immediately repeat.
    void observe_authority_application(const HeroIdentity& identity, int current_state, int incoming_state) noexcept;
    void begin_prediction(const HeroIdentity& identity) noexcept;
    // Restores only button levels proven held across consecutive native samples.
    [[nodiscard]] bool reconcile_input(const HeroIdentity& identity, std::uint8_t buttons,
                                       std::uint8_t& down_mask) noexcept;
    // Identifies the exact prediction transition already performed by authority.
    [[nodiscard]] bool resolve_replay(const HeroIdentity& identity, int target_state) noexcept;
    void finish_prediction(const HeroIdentity& identity) noexcept;

    void clear(const HeroIdentity& identity) noexcept;
    void clear_weapon(std::uintptr_t weapon) noexcept;
    void clear_all() noexcept;

  private:
    struct StateNode {
        std::int32_t turn{-1};
        std::int8_t to_state{-1};
        bool starts_action{};
    };

    struct Slot {
        HeroIdentity identity{};
        std::array<StateNode, kTransitionCapacity> path{};
        std::uint8_t count{};
        std::int8_t predicted_state{-1};
        std::int8_t authority_state{-1};
        std::int32_t authority_turn{-1};
        std::int32_t last_stale_idle_turn{-1};
        std::uint8_t stale_idle_samples{};
        std::uint8_t previous_buttons{};
        std::int8_t replay_state{-1};
        bool authority_seen{};
        bool previous_buttons_valid{};
        bool prediction_window{};
    };

    [[nodiscard]] Slot& bind(const HeroIdentity& identity) noexcept;
    [[nodiscard]] Slot* find(const HeroIdentity& identity) noexcept;
    static void seed_authority(Slot& slot, int state, std::int32_t turn) noexcept;
    static void clear_pending(Slot& slot) noexcept;
    static void consume(Slot& slot, std::size_t count) noexcept;
    [[nodiscard]] static std::size_t find_pending(const Slot& slot, int state) noexcept;
    [[nodiscard]] static int expected_state(const Slot& slot) noexcept;
    [[nodiscard]] static bool unconfirmed_start(const Slot& slot) noexcept;

    std::array<Slot, kNetworkPlayers> slots_{};
};

} // namespace fusioncutter::patches::hero_animation_fix
