#include "crouch_bug_fix.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace fusioncutter::patches::crouch_bug_fix {
namespace {

constexpr std::size_t kCrouchTriggerOffset = 0x48;
constexpr std::size_t kPlayerHandleOffset = 0xD4;

constexpr auto kTriggerCallArguments =
    byte_array<0x8B, 0x4F, 0x04, 0xF3, 0x0F, 0x10, 0x4D, 0x08, 0x83, 0xC1, 0x48, 0x8B, 0x40, 0x10, 0xC1, 0xE8, 0x04,
               0x24, 0x01, 0x0F, 0xB6, 0xC0, 0x50>();
constexpr auto kSteamTriggerCall = byte_array<0xE8, 0x0C, 0xFD, 0xE1, 0xFF>();
constexpr auto kGogTriggerCall = byte_array<0xE8, 0x8C, 0xEC, 0xE1, 0xFF>();
constexpr auto kSteamJoystickLookup =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0xE8, 0x25, 0xB3, 0xFF, 0xFF, 0x5D, 0xC3>();
constexpr auto kGogJoystickLookup =
    byte_array<0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0xE8, 0x15, 0xB3, 0xFF, 0xFF, 0x5D, 0xC3>();

struct TargetData {
    std::uint32_t trigger_call_rva;
    std::span<const std::byte> trigger_call;
    std::uint32_t joystick_lookup_rva;
    std::span<const std::byte> joystick_lookup;
};

constexpr TargetData kSteamTarget{0x0021AC3F, kSteamTriggerCall, 0x001B73C0, kSteamJoystickLookup};
constexpr TargetData kGogTarget{0x0021BCAF, kGogTriggerCall, 0x001B8370, kGogJoystickLookup};

[[nodiscard]] const TargetData& target_data(TargetLayout layout) noexcept {
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

CrouchBugFix::CrouchBugFix(const TargetContext& target) noexcept
    : layout_(target.layout), get_joystick_index_(target.image.function_at_rva<GetJoystickIndex>(
                                  target_data(target.layout).joystick_lookup_rva)) {}

void CrouchBugFix::build_plan(PatchPlan& plan) {
    const auto& target = target_data(layout_);
    plan.require_bytes("Verify crouch call arguments", target.trigger_call_rva - kTriggerCallArguments.size(),
                       BytePattern::exact(kTriggerCallArguments));
    plan.require_bytes("Verify local-player lookup", target.joystick_lookup_rva,
                       BytePattern::exact(target.joystick_lookup));
    plan.mid_hook("Preserve remote crouch input", target.trigger_call_rva, BytePattern::exact(target.trigger_call),
                  &CrouchBugFix::sample_crouch_input);
}

void CrouchBugFix::enable_runtime() noexcept {
    active_.publish(*this);
}

void CrouchBugFix::disable_runtime() noexcept {
    active_.clear(*this);
}

std::uint32_t CrouchBugFix::resolve_input(const void* crouch_trigger, std::uint32_t input_down) const noexcept {
    // Remote movement omits crouch input, so nonlocal soldiers reuse the replicated Trigger level.
    const auto* trigger = static_cast<const std::byte*>(crouch_trigger);
    const auto* controllable = trigger - kCrouchTriggerOffset;

    int player_handle{};
    std::uint32_t trigger_state{};
    std::memcpy(&player_handle, controllable + kPlayerHandleOffset, sizeof(player_handle));
    std::memcpy(&trigger_state, trigger, sizeof(trigger_state));

    const auto locally_controlled = get_joystick_index_(player_handle) >= 0;
    return locally_controlled ? input_down : trigger_state & 1U;
}

void CrouchBugFix::sample_crouch_input(MidHookContext& context) noexcept {
    const auto* patch = active_.read();
    if (patch == nullptr || context.ecx == 0 || context.esp == 0) {
        return;
    }

    // Replace only the Boolean argument consumed by this crouch Trigger::Update call.
    auto* input_down = reinterpret_cast<std::uint32_t*>(context.esp);
    *input_down = patch->resolve_input(reinterpret_cast<const void*>(context.ecx), *input_down);
}

} // namespace fusioncutter::patches::crouch_bug_fix
