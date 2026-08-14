#include "subject.hpp"

#include <algorithm>
#include <cstddef>

namespace fusioncutter::patches::hero_diagnostics {
namespace {

constexpr std::size_t kOwnerOffset = 0x6C;
constexpr std::size_t kComboOffset = 0x114;
constexpr std::size_t kControllableOffset = 0x240;
constexpr std::size_t kPlayerHandleOffset = 0xD4;
constexpr std::size_t kAliveFlagsOffset = 0x1FC;
constexpr std::uint8_t kAliveFlag = 1U << 3U;
constexpr std::size_t kWeaponArrayOffset = 0x720;
constexpr std::size_t kFallbackWeaponIndexOffset = 0x740;
constexpr std::size_t kPackedSelectionOffset = 0x742;
constexpr std::size_t kAnimatorOffset = 0x750;
constexpr int kWeaponSlots = 8;

} // namespace

SubjectTable::SubjectTable(const TargetContext& target, GetJoystickIndex get_joystick_index) noexcept
    : role_(target.role), get_joystick_index_(get_joystick_index) {}

HeroSubject* SubjectTable::bind(void* weapon) noexcept {
    if (weapon == nullptr) {
        return nullptr;
    }
    auto* owner = read_native_field<void*>(weapon, kOwnerOffset);
    auto* combo = read_native_field<void*>(weapon, kComboOffset);
    if (owner == nullptr || combo == nullptr) {
        return nullptr;
    }

    const auto player = read_native_field<int>(owner, kPlayerHandleOffset);
    if (player < 0 || player >= static_cast<int>(subjects_.size())) {
        return nullptr;
    }
    auto* soldier = static_cast<std::byte*>(owner) - kControllableOffset;
    void* animator{};
    if (role_ == HostRole::Client) {
        animator = read_native_field<void*>(soldier, kAnimatorOffset);
        if (animator == nullptr) {
            return nullptr;
        }
    }

    auto& subject = subjects_[static_cast<std::size_t>(player)];
    const auto changed = subject.weapon != weapon || subject.owner != owner || subject.combo != combo ||
                         subject.soldier != soldier || subject.animator != animator;
    if (changed) {
        const auto generation = subject.generation + 1;
        subject = {
            .weapon = weapon,
            .owner = owner,
            .combo = combo,
            .soldier = soldier,
            .animator = animator,
            .id = ++next_id_,
            .generation = generation,
            .player = static_cast<std::int16_t>(player),
        };
        if (role_ == HostRole::Client && get_joystick_index_ != nullptr) {
            const auto local = get_joystick_index_(player);
            if (local >= 0 && local < 2) {
                subject.local_player = static_cast<std::uint8_t>(local);
            }
        }
    }
    return current(subject) ? &subject : nullptr;
}

HeroSubject* SubjectTable::find_weapon(const void* weapon) noexcept {
    const auto match = std::ranges::find(subjects_, weapon, &HeroSubject::weapon);
    return match != subjects_.end() && current(*match) ? &*match : nullptr;
}

HeroSubject* SubjectTable::find_player(int player) noexcept {
    if (player < 0 || player >= static_cast<int>(subjects_.size())) {
        return nullptr;
    }
    auto& subject = subjects_[static_cast<std::size_t>(player)];
    return current(subject) ? &subject : nullptr;
}

HeroSubject* SubjectTable::find_animator(const void* animator) noexcept {
    if (animator == nullptr) {
        return nullptr;
    }
    const auto match = std::ranges::find(subjects_, animator, &HeroSubject::animator);
    return match != subjects_.end() && current(*match) ? &*match : nullptr;
}

HeroSubject* SubjectTable::find_soldier(const void* soldier) noexcept {
    const auto match = std::ranges::find(subjects_, soldier, &HeroSubject::soldier);
    return match != subjects_.end() && current(*match) ? &*match : nullptr;
}

bool SubjectTable::current(const HeroSubject& subject) const noexcept {
    if (subject.weapon == nullptr || subject.owner == nullptr || subject.combo == nullptr ||
        subject.soldier == nullptr || read_native_field<void*>(subject.weapon, kOwnerOffset) != subject.owner ||
        read_native_field<void*>(subject.weapon, kComboOffset) != subject.combo ||
        read_native_field<int>(subject.owner, kPlayerHandleOffset) != subject.player) {
        return false;
    }
    if (role_ == HostRole::Server) {
        return true;
    }
    if (subject.animator == nullptr) {
        return false;
    }
    const auto alive = (read_native_field<std::uint8_t>(subject.soldier, kAliveFlagsOffset) & kAliveFlag) != 0;
    return alive && selected_weapon(subject) == subject.weapon;
}

void SubjectTable::reset() noexcept {
    subjects_ = {};
    next_id_ = 0;
}

void* SubjectTable::selected_weapon(const HeroSubject& subject) const noexcept {
    const auto packed = read_native_field<std::uint8_t>(subject.soldier, kPackedSelectionOffset);
    auto index = static_cast<int>(packed & 0x0F);
    if ((index & 0x08) != 0) {
        index -= 16;
    }
    if (index < 0 || index >= kWeaponSlots) {
        index = read_native_field<std::int8_t>(subject.soldier, kFallbackWeaponIndexOffset);
    }
    return index >= 0 && index < kWeaponSlots
               ? read_native_field<void*>(subject.soldier,
                                          kWeaponArrayOffset + static_cast<std::size_t>(index) * sizeof(void*))
               : nullptr;
}

} // namespace fusioncutter::patches::hero_diagnostics
