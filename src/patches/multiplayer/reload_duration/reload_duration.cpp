#include "reload_duration.hpp"

#include <cstddef>
#include <span>
#include <utility>

namespace fusioncutter::patches::reload_duration {

struct ReloadDurationTargetData {
    std::uint32_t decoded_read_rva;
    std::uint32_t joystick_lookup_rva;
    std::span<const std::byte> joystick_lookup_preimage;
};

namespace {

constexpr std::size_t kWeaponClassOffset = 0x64;
constexpr std::size_t kWeaponOwnerOffset = 0x6C;
constexpr std::size_t kWeaponStateOffset = 0xB0;
constexpr std::size_t kWeaponDurationOffset = 0xB8;
constexpr std::size_t kClassDurationOffset = 0x9C;
constexpr std::size_t kPlayerHandleOffset = 0xD4;
constexpr int kReloadState = 4;

constexpr auto kDecodedReadPreimage =
    byte_array<0x8B, 0x46, 0x6C, 0x80, 0xB8, 0x60, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x84, 0x92, 0x00, 0x00, 0x00>();
constexpr auto kSteamJoystickLookup =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0xE8, 0x25, 0xB3, 0xFF, 0xFF, 0x5D, 0xC3>();
constexpr auto kGogJoystickLookup =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0xE8, 0x15, 0xB3, 0xFF, 0xFF, 0x5D, 0xC3>();

constexpr ReloadDurationTargetData kSteamTarget{0x001E8598, 0x001B73C0, kSteamJoystickLookup};
constexpr ReloadDurationTargetData kGogTarget{0x001E9638, 0x001B8370, kGogJoystickLookup};

[[nodiscard]] const ReloadDurationTargetData& target_data(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
        return kSteamTarget;
    case TargetLayout::GOGRetail:
        return kGogTarget;
    case TargetLayout::Aspyr:
    case TargetLayout::ModTools:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace

ReloadDurationFix::ReloadDurationFix(const TargetContext& target) noexcept
    : target_(target_data(target.layout)),
      get_joystick_index_(target.image.function_at_rva<GetJoystickIndex>(target_.joystick_lookup_rva)) {}

void ReloadDurationFix::build_plan(PatchPlan& plan) {
    plan.require_bytes("Validate local-player lookup", target_.joystick_lookup_rva,
                       BytePattern::exact(target_.joystick_lookup_preimage));
    plan.mid_hook("Repair decoded reload duration", target_.decoded_read_rva, BytePattern::exact(kDecodedReadPreimage),
                  &ReloadDurationFix::inspect_decoded_weapon);
}

void ReloadDurationFix::enable_runtime() noexcept {
    active_.publish(*this);
}

void ReloadDurationFix::disable_runtime() noexcept {
    active_.clear(*this);
}

void ReloadDurationFix::repair_duration(void* weapon) const noexcept {
    if (weapon == nullptr || read_native_field<int>(weapon, kWeaponStateOffset) != kReloadState) {
        return;
    }

    const auto* owner = read_native_field<void*>(weapon, kWeaponOwnerOffset);
    const auto* weapon_class = read_native_field<void*>(weapon, kWeaponClassOffset);
    if (owner == nullptr || weapon_class == nullptr) {
        return;
    }

    const auto player_handle = read_native_field<int>(owner, kPlayerHandleOffset);
    if (get_joystick_index_(player_handle) < 0) {
        return;
    }

    const auto duration = read_native_field<float>(weapon, kWeaponDurationOffset);
    const auto class_duration = read_native_field<float>(weapon_class, kClassDurationOffset);
    if (duration != class_duration) {
        write_native_field(weapon, kWeaponDurationOffset, class_duration);
    }
}

void ReloadDurationFix::inspect_decoded_weapon(MidHookContext& context) noexcept {
    if (auto* patch = active_.read(); patch != nullptr && context.esi != 0) {
        patch->repair_duration(reinterpret_cast<void*>(context.esi));
    }
}

} // namespace fusioncutter::patches::reload_duration
