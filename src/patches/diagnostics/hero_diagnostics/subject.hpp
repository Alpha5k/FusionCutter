#pragma once

#include <FusionCutter/patching.hpp>

#include <array>
#include <cstdint>

namespace fusioncutter::patches::hero_diagnostics {

struct HeroSubject {
    void* weapon{};
    void* owner{};
    void* combo{};
    void* soldier{};
    void* animator{};
    std::uint32_t id{};
    std::uint32_t generation{};
    std::int16_t player{-1};
    std::uint8_t local_player{0xFF};
    bool announced{};
    std::array<std::uint32_t, 6> last_move{};
    std::array<std::uint32_t, 6> last_consumed_move{};
    bool move_seen{};
    bool consumed_move_seen{};
};

// Binds only human melee weapons observed at verified native melee boundaries.
class SubjectTable {
  public:
    using GetJoystickIndex = int(__cdecl*)(int);

    SubjectTable(const TargetContext& target, GetJoystickIndex get_joystick_index) noexcept;

    [[nodiscard]] HeroSubject* bind(void* weapon) noexcept;
    [[nodiscard]] HeroSubject* find_weapon(const void* weapon) noexcept;
    [[nodiscard]] HeroSubject* find_player(int player) noexcept;
    [[nodiscard]] HeroSubject* find_animator(const void* animator) noexcept;
    [[nodiscard]] HeroSubject* find_soldier(const void* soldier) noexcept;
    [[nodiscard]] bool current(const HeroSubject& subject) const noexcept;
    void reset() noexcept;

  private:
    [[nodiscard]] void* selected_weapon(const HeroSubject& subject) const noexcept;

    HostRole role_;
    GetJoystickIndex get_joystick_index_{};
    std::array<HeroSubject, 64> subjects_{};
    std::uint32_t next_id_{};
};

} // namespace fusioncutter::patches::hero_diagnostics
