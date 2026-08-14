#include "../hero_diagnostics.hpp"

#include <FusionCutter/patch.hpp>

#include <array>
#include <cstddef>
#include <cstring>

namespace fusioncutter::patches::hero_diagnostics {
namespace {

using UpdateFunction = bool(__thiscall*)(void*, float);
using EnterStateFunction = void(__thiscall*)(void*, int);
using ExitStateFunction = void(__thiscall*)(void*);
using OverrideVelocityFunction = bool(__thiscall*)(void*, float*, float*, float*, float*);
using WriteFunction = void(__thiscall*)(void*, void*);
using DeflectFunction = bool(__thiscall*)(void*, void*, float*, std::uint32_t);

constexpr std::size_t kStateOffset = 0x180;
constexpr std::size_t kPreviousStateOffset = 0x184;
constexpr std::size_t kStateTimeOffset = 0x188;
constexpr std::size_t kInputTimeOffset = 0x130;
constexpr std::size_t kPositionOffset = 0x120;
constexpr std::size_t kVelocityOffset = 0x4DC;
constexpr std::size_t kPrimaryTriggerOffset = 0x38;
constexpr std::size_t kSecondaryTriggerOffset = 0x3C;
constexpr std::size_t kJumpTriggerOffset = 0x44;
constexpr std::size_t kSprintTriggerOffset = 0x4C;

OriginalFunction<UpdateFunction> gUpdateOriginal;
OriginalFunction<EnterStateFunction> gEnterStateOriginal;
OriginalFunction<ExitStateFunction> gExitStateOriginal;
OriginalFunction<OverrideVelocityFunction> gOverrideVelocityOriginal;
OriginalFunction<WriteFunction> gWriteOriginal;
OriginalFunction<DeflectFunction> gDeflectOriginal;
const std::byte* gMoveHistory{};

[[nodiscard]] std::uint32_t pointer_id(const void* pointer) noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

template <std::size_t Size>
void copy_floats(std::array<float, Size>& destination, const void* object, std::size_t offset) noexcept {
    if (object != nullptr) {
        std::memcpy(destination.data(), static_cast<const std::byte*>(object) + offset, sizeof(destination));
    }
}

[[nodiscard]] std::array<float, 4> vector_arguments(float* first, float* second, float* third, float* fourth) noexcept {
    return {
        first != nullptr ? *first : 0.0F,
        second != nullptr ? *second : 0.0F,
        third != nullptr ? *third : 0.0F,
        fourth != nullptr ? *fourth : 0.0F,
    };
}

void record_server_move(HeroDiagnostics& diagnostics, HeroSubject& subject) noexcept {
    if (diagnostics.capture_mode() == CaptureMode::Standard || gMoveHistory == nullptr || subject.player < 0) {
        return;
    }
    const auto context = diagnostics.turn_context();
    const auto turn = static_cast<std::uint32_t>(context.host_turn - 1) & 0x1F;
    const auto* move = reinterpret_cast<const std::uint32_t*>(gMoveHistory + static_cast<std::size_t>(turn) * 0x600 +
                                                              static_cast<std::size_t>(subject.player) * 0x18);
    std::array<std::uint32_t, 6> move_values{};
    std::memcpy(move_values.data(), move, sizeof(move_values));
    const auto changed = !subject.consumed_move_seen || move_values != subject.last_consumed_move;
    subject.last_consumed_move = move_values;
    subject.consumed_move_seen = true;
    trace::NetworkMovementRecord record{
        .turn = context,
        .axes = {move_values[0], move_values[1], move_values[2], move_values[3]},
        .buttons = move_values[4],
        .triggers =
            {
                read_native_field<std::uint32_t>(subject.owner, kPrimaryTriggerOffset),
                read_native_field<std::uint32_t>(subject.owner, kSecondaryTriggerOffset),
                read_native_field<std::uint32_t>(subject.owner, kSprintTriggerOffset),
                read_native_field<std::uint32_t>(subject.owner, kJumpTriggerOffset),
            },
        .state = read_native_field<int>(subject.weapon, kStateOffset),
        .kind = trace::NetworkMovementKind::ServerConsumed,
        .changed = static_cast<std::uint8_t>(changed),
    };
    copy_floats(record.position, subject.soldier, kPositionOffset);
    copy_floats(record.velocity, subject.soldier, kVelocityOffset);
    diagnostics.submit(trace::RecordKind::NetworkMovement, trace::payload_bytes(record), subject,
                       trace::RecordFlags::Authority);
}

bool __fastcall hook_update(void* weapon, void*, float delta) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->bind(weapon) : nullptr;
    trace::MeleeTickRecord record{};
    if (subject != nullptr) {
        record = {
            .turn = diagnostics->turn_context(),
            .delta = delta,
            .state_time_before = read_native_field<float>(weapon, kStateTimeOffset),
            .input_time_before = read_native_field<float>(weapon, kInputTimeOffset),
            .state_fingerprint = diagnostics->state_fingerprint(*subject),
            .state_before = static_cast<std::int8_t>(read_native_field<int>(weapon, kStateOffset)),
            .previous_before = static_cast<std::int8_t>(read_native_field<int>(weapon, kPreviousStateOffset)),
        };
        record_server_move(*diagnostics, *subject);
    }
    const auto original = gUpdateOriginal.get();
    const auto result = original != nullptr && original(weapon, delta);
    if (subject != nullptr) {
        record.state_time_after = read_native_field<float>(weapon, kStateTimeOffset);
        record.input_time_after = read_native_field<float>(weapon, kInputTimeOffset);
        record.state_after = static_cast<std::int8_t>(read_native_field<int>(weapon, kStateOffset));
        record.previous_after = static_cast<std::int8_t>(read_native_field<int>(weapon, kPreviousStateOffset));
        record.native_called = 1;
        record.result = static_cast<std::uint8_t>(result);
        copy_floats(record.position, subject->soldier, kPositionOffset);
        copy_floats(record.velocity, subject->soldier, kVelocityOffset);
        auto flags = static_cast<std::uint16_t>(trace::RecordFlags::Authority | trace::RecordFlags::NativeCalled);
        if (result) {
            flags |= trace::RecordFlags::NativeResult;
        }
        if (record.state_before != record.state_after) {
            flags |= trace::RecordFlags::StateChanged;
        }
        diagnostics->submit(trace::RecordKind::MeleeTick, trace::payload_bytes(record), *subject, flags);
    }
    return result;
}

void __fastcall hook_enter_state(void* weapon, void*, int state) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->bind(weapon) : nullptr;
    trace::TransitionRecord record{};
    if (subject != nullptr) {
        record = {
            .turn = diagnostics->turn_context(),
            .requested_state = state,
            .state_before = read_native_field<int>(weapon, kStateOffset),
            .state_after = -1,
            .previous_state = read_native_field<int>(weapon, kPreviousStateOffset),
            .state_time_before = read_native_field<float>(weapon, kStateTimeOffset),
            .kind = trace::TransitionKind::Enter,
        };
    }
    if (const auto original = gEnterStateOriginal.get(); original != nullptr) {
        original(weapon, state);
    }
    if (subject != nullptr) {
        record.state_after = read_native_field<int>(weapon, kStateOffset);
        record.previous_state = read_native_field<int>(weapon, kPreviousStateOffset);
        record.state_time_after = read_native_field<float>(weapon, kStateTimeOffset);
        record.state_fingerprint = diagnostics->state_fingerprint(*subject);
        copy_floats(record.position, subject->soldier, kPositionOffset);
        copy_floats(record.velocity, subject->soldier, kVelocityOffset);
        auto flags = static_cast<std::uint16_t>(trace::RecordFlags::Authority | trace::RecordFlags::NativeCalled);
        if (record.state_before != record.state_after) {
            flags |= trace::RecordFlags::StateChanged;
        }
        diagnostics->submit(trace::RecordKind::Transition, trace::payload_bytes(record), *subject, flags);
    }
}

void __fastcall hook_exit_state(void* weapon, void*) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->bind(weapon) : nullptr;
    trace::TransitionRecord record{};
    if (subject != nullptr) {
        record = {
            .turn = diagnostics->turn_context(),
            .requested_state = read_native_field<int>(weapon, kStateOffset),
            .state_before = read_native_field<int>(weapon, kStateOffset),
            .state_after = -1,
            .previous_state = read_native_field<int>(weapon, kPreviousStateOffset),
            .state_time_before = read_native_field<float>(weapon, kStateTimeOffset),
            .kind = trace::TransitionKind::Exit,
        };
    }
    if (const auto original = gExitStateOriginal.get(); original != nullptr) {
        original(weapon);
    }
    if (subject != nullptr) {
        record.state_after = read_native_field<int>(weapon, kStateOffset);
        record.previous_state = read_native_field<int>(weapon, kPreviousStateOffset);
        record.state_time_after = read_native_field<float>(weapon, kStateTimeOffset);
        record.state_fingerprint = diagnostics->state_fingerprint(*subject);
        diagnostics->submit(trace::RecordKind::Transition, trace::payload_bytes(record), *subject,
                            trace::RecordFlags::Authority | trace::RecordFlags::NativeCalled);
    }
}

bool __fastcall hook_override_velocity(void* weapon, void*, float* first, float* second, float* third,
                                       float* fourth) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->bind(weapon) : nullptr;
    trace::MovementFrameRecord record{};
    if (subject != nullptr) {
        record.turn = diagnostics->turn_context();
        record.before = vector_arguments(first, second, third, fourth);
        record.state = read_native_field<int>(weapon, kStateOffset);
        record.kind = trace::MovementKind::Velocity;
    }
    const auto original = gOverrideVelocityOriginal.get();
    const auto result = original != nullptr && original(weapon, first, second, third, fourth);
    if (subject != nullptr) {
        record.after = vector_arguments(first, second, third, fourth);
        copy_floats(record.position, subject->soldier, kPositionOffset);
        copy_floats(record.velocity, subject->soldier, kVelocityOffset);
        record.result = static_cast<std::uint8_t>(result);
        auto flags = static_cast<std::uint16_t>(trace::RecordFlags::Authority | trace::RecordFlags::NativeCalled);
        if (result) {
            flags |= trace::RecordFlags::NativeResult;
        }
        diagnostics->submit(trace::RecordKind::MovementFrame, trace::payload_bytes(record), *subject, flags);
    }
    return result;
}

void __fastcall hook_write(void* weapon, void*, void* stream) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->bind(weapon) : nullptr;
    trace::NetworkStateRecord record{};
    if (subject != nullptr) {
        const auto state = read_native_field<int>(weapon, kStateOffset);
        record = {
            .turn = diagnostics->turn_context(),
            .state_before = state,
            .requested_state = state,
            .state_after = state,
            .state_time_before = read_native_field<float>(weapon, kStateTimeOffset),
            .state_time_after = read_native_field<float>(weapon, kStateTimeOffset),
            .state_fingerprint = diagnostics->state_fingerprint(*subject),
            .stream = pointer_id(stream),
            .action = read_native_field<std::uint16_t>(weapon, 0x17C),
            .state_flags = read_native_field<std::uint8_t>(weapon, 0x179),
            .native_called = 1,
            .operation = trace::NetworkStateOperation::Write,
        };
    }
    if (const auto original = gWriteOriginal.get(); original != nullptr) {
        original(weapon, stream);
    }
    if (subject != nullptr) {
        diagnostics->submit(trace::RecordKind::NetworkState, trace::payload_bytes(record), *subject,
                            trace::RecordFlags::Authority | trace::RecordFlags::NativeCalled);
    }
}

bool __fastcall hook_deflect(void* weapon, void*, void* target, float* position, std::uint32_t filter) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->bind(weapon) : nullptr;
    trace::MeleeContactRecord contact{};
    trace::DeflectionRecord deflection{};
    if (subject != nullptr) {
        contact = {
            .turn = diagnostics->turn_context(),
            .target = pointer_id(target),
            .filter = filter,
            .state = read_native_field<int>(weapon, kStateOffset),
            .state_fingerprint = diagnostics->state_fingerprint(*subject),
        };
        deflection = {.turn = contact.turn, .target = contact.target, .state = contact.state};
        if (position != nullptr) {
            std::memcpy(contact.position.data(), position, sizeof(contact.position));
            deflection.input_position = contact.position;
        }
        diagnostics->submit(trace::RecordKind::MeleeContact, trace::payload_bytes(contact), *subject,
                            trace::RecordFlags::Begin | trace::RecordFlags::Authority);
    }
    const auto original = gDeflectOriginal.get();
    const auto result = original != nullptr && original(weapon, target, position, filter);
    if (subject != nullptr) {
        if (position != nullptr) {
            std::memcpy(deflection.output_position.data(), position, sizeof(deflection.output_position));
        }
        deflection.result = static_cast<std::uint8_t>(result);
        auto flags = static_cast<std::uint16_t>(trace::RecordFlags::End | trace::RecordFlags::Authority |
                                                trace::RecordFlags::NativeCalled);
        if (result) {
            flags |= trace::RecordFlags::NativeResult;
        }
        diagnostics->submit(trace::RecordKind::Deflection, trace::payload_bytes(deflection), *subject, flags);
    }
    return result;
}

} // namespace

void build_server_plan(PatchPlan& plan, const TargetContext& target, CaptureMode mode) {
    const auto& layout = server_layout();
    gUpdateOriginal = plan.inline_hook_with_original<UpdateFunction>("Observe authoritative melee updates",
                                                                     layout.update.rva, layout.update.pattern(),
                                                                     reinterpret_cast<UpdateFunction>(&hook_update));
    gEnterStateOriginal = plan.inline_hook_with_original<EnterStateFunction>(
        "Observe authoritative melee entries", layout.enter_state.rva, layout.enter_state.pattern(),
        reinterpret_cast<EnterStateFunction>(&hook_enter_state));
    gExitStateOriginal = plan.inline_hook_with_original<ExitStateFunction>(
        "Observe authoritative melee exits", layout.exit_state.rva, layout.exit_state.pattern(),
        reinterpret_cast<ExitStateFunction>(&hook_exit_state));
    gOverrideVelocityOriginal = plan.inline_hook_with_original<OverrideVelocityFunction>(
        "Observe authoritative melee velocity", layout.override_velocity.rva, layout.override_velocity.pattern(),
        reinterpret_cast<OverrideVelocityFunction>(&hook_override_velocity));
    gWriteOriginal = plan.inline_hook_with_original<WriteFunction>("Observe serialized melee state", layout.write.rva,
                                                                   layout.write.pattern(),
                                                                   reinterpret_cast<WriteFunction>(&hook_write));
    if (mode != CaptureMode::Standard) {
        gDeflectOriginal = plan.inline_hook_with_original<DeflectFunction>(
            "Observe authoritative melee deflections", layout.deflect.rva, layout.deflect.pattern(),
            reinterpret_cast<DeflectFunction>(&hook_deflect));
    }
    gMoveHistory = target.image.read_at_rva<std::byte>(layout.move_history_rva);
}

} // namespace fusioncutter::patches::hero_diagnostics
