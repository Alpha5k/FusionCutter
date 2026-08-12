#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::weapon_swap_replay {

inline constexpr int kLocalPlayers = 2;
inline constexpr int kWeaponChannels = 2;
inline constexpr int kWeaponIndices = 8;
inline constexpr std::uint32_t kPositiveSwitchDirectionBits = 0x3F80'0000U;
inline constexpr std::uint32_t kNegativeSwitchDirectionBits = 0xBF80'0000U;
inline constexpr std::array<std::uint32_t, kWeaponChannels> kPositiveSwitchButtons{0x2000U, 0x8000U};
inline constexpr std::array<std::uint32_t, kWeaponChannels> kNegativeSwitchButtons{0x4000U, 0x1'0000U};

[[nodiscard]] constexpr int packed_weapon_index(std::uint8_t packed) noexcept {
    auto index = static_cast<int>(packed & 0x0F);
    return (index & 0x08) == 0 ? index : index - 16;
}

[[nodiscard]] constexpr int packed_weapon_channel(std::uint8_t packed) noexcept {
    auto channel = static_cast<int>((packed >> 4) & 0x03);
    return (channel & 0x02) == 0 ? channel : channel - 4;
}

struct TurnFrontiers {
    std::int32_t update{-1};
    std::int32_t predict{-1};
    std::int32_t acknowledged{-1};
};

// Signed half-range comparisons preserve ordering when the 32-bit turn wraps.
[[nodiscard]] constexpr bool turn_at_or_after(std::int32_t turn, std::int32_t frontier) noexcept {
    if (turn < 0 || frontier < 0) {
        return false;
    }
    const auto difference = static_cast<std::uint32_t>(turn) - static_cast<std::uint32_t>(frontier);
    return static_cast<std::int32_t>(difference) >= 0;
}

[[nodiscard]] constexpr bool turn_after(std::int32_t turn, std::int32_t frontier) noexcept {
    if (turn < 0 || frontier < 0) {
        return false;
    }
    const auto difference = static_cast<std::uint32_t>(turn) - static_cast<std::uint32_t>(frontier);
    return static_cast<std::int32_t>(difference) > 0;
}

[[nodiscard]] constexpr bool turn_distance_greater(std::int32_t turn, std::int32_t frontier,
                                                   std::int32_t distance) noexcept {
    if (turn < 0 || frontier < 0) {
        return false;
    }
    const auto difference = static_cast<std::uint32_t>(turn) - static_cast<std::uint32_t>(frontier);
    return static_cast<std::int32_t>(difference) >= 0 && difference > static_cast<std::uint32_t>(distance);
}

inline void advance_turn(std::int32_t& high, std::int32_t observed) noexcept {
    if (observed >= 0 && (high < 0 || turn_after(observed, high))) {
        high = observed;
    }
}

enum class RequestKind : std::uint8_t {
    Invalid,
    New,
    Replay,
    HistoricalConflict,
    RestartedConflict,
    RestartedDiscontinuity,
    RestartedOverflow,
};

enum class AuthoritativeKind : std::uint8_t {
    Native,
    Pending,
    Accepted,
    Rejected,
    Correction,
};

struct RequestDecision {
    RequestKind kind{RequestKind::Invalid};
    std::uint32_t epoch{};

    [[nodiscard]] bool valid() const noexcept {
        return kind != RequestKind::Invalid;
    }

    [[nodiscard]] bool replay() const noexcept {
        return kind == RequestKind::Replay;
    }

    [[nodiscard]] bool failed_open() const noexcept {
        return kind == RequestKind::HistoricalConflict;
    }
};

struct AuthoritativeDecision {
    AuthoritativeKind kind{AuthoritativeKind::Native};
    std::uint32_t epoch{};
    std::uint32_t sequence{};
    bool mute_select{};
    bool pointer_invalidated{};
};

struct EpochSnapshot {
    void* soldier{};
    std::array<void*, kWeaponIndices> node_weapons{};
    std::uint8_t node_mask{};
    std::uint8_t final_index{};
    std::int32_t latest_request_turn{-1};
    std::uint32_t epoch{};
    std::uint32_t sequence{};
};

// Recognizes active prediction and the zeroed frontier state used during disconnect or reset.
class NetworkLifecycle {
  public:
    [[nodiscard]] bool observe(const TurnFrontiers& frontiers) noexcept {
        const bool active_now = frontiers.update > 0 && frontiers.predict > 0 && frontiers.acknowledged >= 0;
        const bool reset_state = frontiers.update <= 0 && frontiers.predict <= 0 && frontiers.acknowledged <= 0;
        const bool reset = active_ && reset_state;
        if (reset) {
            active_ = false;
        }
        if (active_now) {
            active_ = true;
        }
        return reset;
    }

    void clear() noexcept {
        active_ = false;
    }

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

  private:
    bool active_{};
};

// Carries duplicate identity from one observed selection path into its immediate Weapon::Select call.
class SelectIntent {
  public:
    void arm(void* expected_weapon, int channel, bool duplicate) noexcept {
        expected_weapon_ = expected_weapon;
        channel_ = channel;
        duplicate_ = duplicate;
    }

    [[nodiscard]] bool consume_duplicate(void* weapon, int channel) noexcept {
        const bool matched = expected_weapon_ == weapon && channel_ == channel;
        const bool duplicate = matched && duplicate_;
        clear();
        return duplicate;
    }

    void clear() noexcept {
        expected_weapon_ = nullptr;
        channel_ = -1;
        duplicate_ = false;
    }

  private:
    void* expected_weapon_{};
    int channel_{-1};
    bool duplicate_{};
};

[[nodiscard]] constexpr std::uint32_t switch_button_mask(int channel, std::uint32_t direction) noexcept {
    if (channel < 0 || channel >= kWeaponChannels) {
        return 0;
    }
    if (direction == kPositiveSwitchDirectionBits) {
        return kPositiveSwitchButtons[static_cast<std::size_t>(channel)];
    }
    if (direction == kNegativeSwitchDirectionBits) {
        return kNegativeSwitchButtons[static_cast<std::size_t>(channel)];
    }
    return 0;
}

[[nodiscard]] constexpr bool held_switch_edge(int channel, std::uint32_t trigger, std::uint32_t direction,
                                              std::uint32_t previous_buttons, std::uint32_t current_buttons) noexcept {
    const auto mask = switch_button_mask(channel, direction);
    return (trigger & 3U) == 3U && mask != 0 && (previous_buttons & mask) != 0 && (current_buttons & mask) != 0;
}

static_assert(packed_weapon_index(0x12) == 2);
static_assert(packed_weapon_channel(0x12) == 1);

} // namespace fusioncutter::patches::weapon_swap_replay
