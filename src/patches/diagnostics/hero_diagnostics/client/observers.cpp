#include "../hero_diagnostics.hpp"

#include <FusionCutter/patch.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>

namespace fusioncutter::patches::hero_diagnostics {
namespace {

using ReadFunction = void(__thiscall*)(void*, void*);
using ExitStateFunction = void(__thiscall*)(void*);
using ActionStateFunction = void(__thiscall*)(void*, std::uint32_t, const float*, float);
using SetupPoseFunction = void(__thiscall*)(void*, void*);
using OverrideControlsFunction = bool(__thiscall*)(void*);
using OverrideVelocityFunction = bool(__thiscall*)(void*, float*, float*, float*, float*);
using SoundPlayFunction = void(__thiscall*)(void*, void*, void*, std::uint32_t, std::uint32_t);
using DeflectFunction = bool(__thiscall*)(void*, void*, float*, std::uint32_t);

constexpr std::size_t kStateOffset = 0x180;
constexpr std::size_t kPreviousStateOffset = 0x184;
constexpr std::size_t kStateTimeOffset = 0x188;
constexpr std::size_t kPositionOffset = 0x120;
constexpr std::size_t kVelocityOffset = 0x4DC;
constexpr std::size_t kControlOffset = 0x80;
constexpr std::size_t kPrimaryTriggerOffset = 0x38;
constexpr std::size_t kSecondaryTriggerOffset = 0x3C;
constexpr std::size_t kJumpTriggerOffset = 0x44;
constexpr std::size_t kSprintTriggerOffset = 0x4C;
constexpr std::size_t kRemoteMoveStride = 0x18;

OriginalFunction<ReadFunction> gReadOriginal;
OriginalFunction<ExitStateFunction> gExitStateOriginal;
OriginalFunction<ActionStateFunction> gActionStateOriginal;
OriginalFunction<SetupPoseFunction> gSetupPoseOriginal;
OriginalFunction<OverrideControlsFunction> gOverrideControlsOriginal;
OriginalFunction<OverrideVelocityFunction> gOverrideVelocityOriginal;
OriginalFunction<SoundPlayFunction> gSoundPlayOriginal;
OriginalFunction<DeflectFunction> gDeflectOriginal;
const std::byte* gRemoteMoves{};
const volatile float* gOuterDelta{};

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

[[nodiscard]] trace::PresentationFrameRecord
presentation_sample(HeroDiagnostics& diagnostics, const HeroSubject& subject, trace::PresentationKind kind) noexcept {
    auto* animator = subject.animator;
    return {
        .turn = diagnostics.turn_context(),
        .state_time = read_native_field<float>(subject.weapon, kStateTimeOffset),
        .frame_delta = gOuterDelta != nullptr ? *gOuterDelta : 0.0F,
        .extra_delta = read_native_field<float>(animator, 0xCC),
        .upper_time = read_native_field<float>(animator, 0x1600),
        .lower_time = read_native_field<float>(animator, 0x1FB0),
        .upper_timer = read_native_field<float>(animator, 0x1FD0),
        .lower_timer = read_native_field<float>(animator, 0x1FCC),
        .upper_blend = read_native_field<float>(animator, 0x1FE4),
        .lower_blend = read_native_field<float>(animator, 0x1FE0),
        .animation_rate = read_native_field<float>(animator, 0x1FFC),
        .upper_clip = read_native_field<std::uint32_t>(animator, 0x15E4),
        .lower_clip = read_native_field<std::uint32_t>(animator, 0x1F94),
        .action = read_native_field<std::uint32_t>(animator, 0x1FEC),
        .weapon_animation = read_native_field<std::uint32_t>(animator, 0x2014),
        .state = static_cast<std::int8_t>(read_native_field<int>(subject.weapon, kStateOffset)),
        .previous_state = static_cast<std::int8_t>(read_native_field<int>(subject.weapon, kPreviousStateOffset)),
        .kind = kind,
        .status = static_cast<std::uint8_t>(
            (read_native_field<std::uint8_t>(animator, 0x1610) != 0 ? trace::UpperAnimationFinished : 0) |
            (read_native_field<std::uint8_t>(animator, 0x1FC0) != 0 ? trace::LowerAnimationFinished : 0)),
    };
}

void __fastcall hook_read(void* weapon, void*, void* stream) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->bind(weapon) : nullptr;
    trace::NetworkStateRecord record{};
    std::uint32_t previous_scope{};
    if (subject != nullptr) {
        previous_scope = diagnostics->begin_read_scope();
        record = {
            .turn = diagnostics->turn_context(),
            .state_before = read_native_field<int>(weapon, kStateOffset),
            .requested_state = -1,
            .state_after = -1,
            .state_time_before = read_native_field<float>(weapon, kStateTimeOffset),
            .state_fingerprint = diagnostics->state_fingerprint(*subject),
            .scope = diagnostics->current_read_scope(),
            .stream = pointer_id(stream),
        };
    }
    if (const auto original = gReadOriginal.get(); original != nullptr) {
        original(weapon, stream);
    }
    if (subject != nullptr) {
        diagnostics->end_read_scope(previous_scope);
        record.state_after = read_native_field<int>(weapon, kStateOffset);
        record.state_time_after = read_native_field<float>(weapon, kStateTimeOffset);
        record.action = read_native_field<std::uint16_t>(weapon, 0x17C);
        record.state_flags = read_native_field<std::uint8_t>(weapon, 0x179);
        record.native_called = 1;
        record.operation = trace::NetworkStateOperation::Read;
        auto flags = static_cast<std::uint16_t>(trace::RecordFlags::Authority | trace::RecordFlags::NativeCalled);
        if (record.state_before != record.state_after) {
            flags |= trace::RecordFlags::StateChanged;
        }
        diagnostics->submit(trace::RecordKind::NetworkState, trace::payload_bytes(record), *subject, flags);
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
        copy_floats(record.position, subject->soldier, kPositionOffset);
        copy_floats(record.velocity, subject->soldier, kVelocityOffset);
        auto flags = static_cast<std::uint16_t>(trace::RecordFlags::NativeCalled);
        flags |= diagnostics->transition_flags(weapon, record.turn);
        if (record.state_before != record.state_after) {
            flags |= trace::RecordFlags::StateChanged;
        }
        diagnostics->submit(trace::RecordKind::Transition, trace::payload_bytes(record), *subject, flags);
    }
}

void __fastcall hook_action_state(void* animator, void*, std::uint32_t state, const float* matrix,
                                  float blend) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->find_animator(animator) : nullptr;
    if (subject != nullptr) {
        auto record = presentation_sample(*diagnostics, *subject, trace::PresentationKind::Action);
        record.argument0 = state;
        record.argument1 = std::bit_cast<std::uint32_t>(blend);
        diagnostics->submit(trace::RecordKind::PresentationFrame, trace::payload_bytes(record), *subject,
                            trace::RecordFlags::Begin);
    }
    if (const auto original = gActionStateOriginal.get(); original != nullptr) {
        original(animator, state, matrix, blend);
    }
    if (subject != nullptr) {
        auto record = presentation_sample(*diagnostics, *subject, trace::PresentationKind::Action);
        record.argument0 = state;
        record.argument1 = std::bit_cast<std::uint32_t>(blend);
        diagnostics->submit(trace::RecordKind::PresentationFrame, trace::payload_bytes(record), *subject,
                            trace::RecordFlags::End | trace::RecordFlags::NativeCalled);
    }
}

void __fastcall hook_setup_pose(void* animator, void*, void* pose) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->find_animator(animator) : nullptr;
    if (subject != nullptr) {
        auto record = presentation_sample(*diagnostics, *subject, trace::PresentationKind::Pose);
        record.argument0 = pointer_id(pose);
        diagnostics->submit(trace::RecordKind::PresentationFrame, trace::payload_bytes(record), *subject,
                            trace::RecordFlags::Begin);
    }
    if (const auto original = gSetupPoseOriginal.get(); original != nullptr) {
        original(animator, pose);
    }
    if (subject != nullptr) {
        auto record = presentation_sample(*diagnostics, *subject, trace::PresentationKind::Pose);
        record.argument0 = pointer_id(pose);
        diagnostics->submit(trace::RecordKind::PresentationFrame, trace::payload_bytes(record), *subject,
                            trace::RecordFlags::End | trace::RecordFlags::NativeCalled);
    }
}

bool __fastcall hook_override_controls(void* weapon, void*) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->bind(weapon) : nullptr;
    trace::MovementFrameRecord record{};
    if (subject != nullptr) {
        record.turn = diagnostics->turn_context();
        copy_floats(record.before, subject->owner, kControlOffset);
        record.state = read_native_field<int>(weapon, kStateOffset);
        record.kind = trace::MovementKind::Controls;
    }
    const auto original = gOverrideControlsOriginal.get();
    const auto result = original != nullptr && original(weapon);
    if (subject != nullptr) {
        copy_floats(record.after, subject->owner, kControlOffset);
        copy_floats(record.position, subject->soldier, kPositionOffset);
        copy_floats(record.velocity, subject->soldier, kVelocityOffset);
        record.result = static_cast<std::uint8_t>(result);
        const auto flags = static_cast<std::uint16_t>(trace::RecordFlags::NativeCalled |
                                                      (result ? trace::RecordFlags::NativeResult : 0));
        diagnostics->submit(trace::RecordKind::MovementFrame, trace::payload_bytes(record), *subject, flags);
    }
    return result;
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
        const auto flags = static_cast<std::uint16_t>(trace::RecordFlags::NativeCalled |
                                                      (result ? trace::RecordFlags::NativeResult : 0));
        diagnostics->submit(trace::RecordKind::MovementFrame, trace::payload_bytes(record), *subject, flags);
    }
    return result;
}

void __fastcall hook_sound_play(void* sound, void*, void* first, void* second, std::uint32_t third,
                                std::uint32_t fourth) noexcept {
    auto* diagnostics = active_diagnostics();
    auto* subject = diagnostics != nullptr ? diagnostics->active_transition_subject() : nullptr;
    if (subject != nullptr) {
        const trace::AudioCueRecord record{
            .turn = diagnostics->turn_context(),
            .sound = pointer_id(sound),
            .argument0 = pointer_id(first),
            .argument1 = pointer_id(second),
            .argument2 = third,
            .argument3 = fourth,
            .state = read_native_field<int>(subject->weapon, kStateOffset),
        };
        diagnostics->submit(trace::RecordKind::AudioCue, trace::payload_bytes(record), *subject);
    }
    if (const auto original = gSoundPlayOriginal.get(); original != nullptr) {
        original(sound, first, second, third, fourth);
    }
}

void observe_parsed_player_move(MidHookContext& context) noexcept {
    auto* diagnostics = active_diagnostics();
    if (diagnostics == nullptr || gRemoteMoves == nullptr || context.ebp < sizeof(std::int32_t)) {
        return;
    }

    std::int32_t player{};
    std::memcpy(&player, reinterpret_cast<const void*>(context.ebp - sizeof(player)), sizeof(player));
    auto* subject = diagnostics->find_player(player);
    if (subject == nullptr || subject->local_player != 0xFF) {
        return;
    }

    std::array<std::uint32_t, 6> move{};
    std::memcpy(move.data(), gRemoteMoves + static_cast<std::size_t>(player) * kRemoteMoveStride, sizeof(move));
    const auto changed = !subject->move_seen || move != subject->last_move;
    subject->last_move = move;
    subject->move_seen = true;
    trace::NetworkMovementRecord record{
        .turn = diagnostics->turn_context(),
        .axes = {move[0], move[1], move[2], move[3]},
        .buttons = move[4],
        .triggers =
            {
                read_native_field<std::uint32_t>(subject->owner, kPrimaryTriggerOffset),
                read_native_field<std::uint32_t>(subject->owner, kSecondaryTriggerOffset),
                read_native_field<std::uint32_t>(subject->owner, kSprintTriggerOffset),
                read_native_field<std::uint32_t>(subject->owner, kJumpTriggerOffset),
            },
        .state = read_native_field<int>(subject->weapon, kStateOffset),
        .kind = trace::NetworkMovementKind::Parsed,
        .changed = static_cast<std::uint8_t>(changed),
    };
    copy_floats(record.position, subject->soldier, kPositionOffset);
    copy_floats(record.velocity, subject->soldier, kVelocityOffset);
    diagnostics->submit(trace::RecordKind::NetworkMovement, trace::payload_bytes(record), *subject,
                        trace::RecordFlags::Authority);
}

// Records paired hero movement snapshots around authoritative Soldier state installation.
void before_soldier_read(void* context, const soldier_state_pipeline::ReadContext& read,
                         soldier_state_pipeline::ObserverState& state) noexcept {
    auto& diagnostics = *static_cast<HeroDiagnostics*>(context);
    auto* subject = diagnostics.find_soldier(read.soldier);
    state.first = reinterpret_cast<std::uintptr_t>(subject);
    if (subject == nullptr) {
        return;
    }

    trace::MovementFrameRecord record{};
    record.turn = diagnostics.turn_context();
    copy_floats(record.position, subject->soldier, kPositionOffset);
    copy_floats(record.velocity, subject->soldier, kVelocityOffset);
    record.state = read_native_field<int>(subject->weapon, kStateOffset);
    record.kind = trace::MovementKind::SoldierRead;
    diagnostics.submit(trace::RecordKind::MovementFrame, trace::payload_bytes(record), *subject,
                       trace::RecordFlags::Begin | trace::RecordFlags::Authority);
}

void after_soldier_read(void* context, const soldier_state_pipeline::ReadContext&,
                        const soldier_state_pipeline::ObserverState& state) noexcept {
    auto& diagnostics = *static_cast<HeroDiagnostics*>(context);
    auto* subject = reinterpret_cast<HeroSubject*>(static_cast<std::uintptr_t>(state.first));
    if (subject == nullptr) {
        return;
    }

    trace::MovementFrameRecord record{
        .turn = diagnostics.turn_context(),
        .state = read_native_field<int>(subject->weapon, kStateOffset),
        .kind = trace::MovementKind::SoldierRead,
    };
    copy_floats(record.position, subject->soldier, kPositionOffset);
    copy_floats(record.velocity, subject->soldier, kVelocityOffset);
    diagnostics.submit(trace::RecordKind::MovementFrame, trace::payload_bytes(record), *subject,
                       trace::RecordFlags::End | trace::RecordFlags::Authority | trace::RecordFlags::NativeCalled);
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
        deflection = {
            .turn = contact.turn,
            .target = contact.target,
            .state = contact.state,
        };
        if (position != nullptr) {
            std::memcpy(contact.position.data(), position, sizeof(contact.position));
            deflection.input_position = contact.position;
        }
        diagnostics->submit(trace::RecordKind::MeleeContact, trace::payload_bytes(contact), *subject,
                            trace::RecordFlags::Begin);
    }
    const auto original = gDeflectOriginal.get();
    const auto result = original != nullptr && original(weapon, target, position, filter);
    if (subject != nullptr) {
        if (position != nullptr) {
            std::memcpy(deflection.output_position.data(), position, sizeof(deflection.output_position));
        }
        deflection.result = static_cast<std::uint8_t>(result);
        auto record_flags = static_cast<std::uint16_t>(trace::RecordFlags::End | trace::RecordFlags::NativeCalled);
        if (result) {
            record_flags |= trace::RecordFlags::NativeResult;
        }
        diagnostics->submit(trace::RecordKind::Deflection, trace::payload_bytes(deflection), *subject, record_flags);
    }
    return result;
}

} // namespace

void build_client_plan(PatchPlan& plan, const TargetContext& target, CaptureMode mode) {
    const auto& layout = client_layout_for(target.layout);
    gReadOriginal =
        plan.inline_hook_with_original<ReadFunction>("Observe melee network reads", layout.read.rva,
                                                     layout.read.pattern(), reinterpret_cast<ReadFunction>(&hook_read));
    gExitStateOriginal = plan.inline_hook_with_original<ExitStateFunction>(
        "Observe melee state exits", layout.exit_state.rva, layout.exit_state.pattern(),
        reinterpret_cast<ExitStateFunction>(&hook_exit_state));
    gOverrideControlsOriginal = plan.inline_hook_with_original<OverrideControlsFunction>(
        "Observe melee control overrides", layout.override_controls.rva, layout.override_controls.pattern(),
        reinterpret_cast<OverrideControlsFunction>(&hook_override_controls));
    gOverrideVelocityOriginal = plan.inline_hook_with_original<OverrideVelocityFunction>(
        "Observe melee velocity overrides", layout.override_velocity.rva, layout.override_velocity.pattern(),
        reinterpret_cast<OverrideVelocityFunction>(&hook_override_velocity));
    if (mode != CaptureMode::Standard) {
        gSoundPlayOriginal = plan.inline_hook_with_original<SoundPlayFunction>(
            "Observe melee transition sounds", layout.sound_play.rva, layout.sound_play.pattern(),
            reinterpret_cast<SoundPlayFunction>(&hook_sound_play));
        plan.mid_hook("Observe parsed remote hero input", layout.player_move_parsed.rva,
                      layout.player_move_parsed.pattern(), &observe_parsed_player_move);
        gDeflectOriginal = plan.inline_hook_with_original<DeflectFunction>(
            "Observe melee deflections", layout.deflect.rva, layout.deflect.pattern(),
            reinterpret_cast<DeflectFunction>(&hook_deflect));
    }
    if (mode == CaptureMode::Full) {
        gActionStateOriginal = plan.inline_hook_with_original<ActionStateFunction>(
            "Observe hero animation actions", layout.action_state.rva, layout.action_state.pattern(),
            reinterpret_cast<ActionStateFunction>(&hook_action_state));
        gSetupPoseOriginal = plan.inline_hook_with_original<SetupPoseFunction>(
            "Observe presented hero poses", layout.setup_pose.rva, layout.setup_pose.pattern(),
            reinterpret_cast<SetupPoseFunction>(&hook_setup_pose));
    }
    gRemoteMoves = target.image.read_at_rva<std::byte>(layout.remote_moves_rva);
    gOuterDelta = target.image.read_at_rva<volatile float>(layout.outer_delta_rva);
}

soldier_state_pipeline::ObserverCallbacks make_soldier_state_observer(HeroDiagnostics& diagnostics) noexcept {
    return {
        .context = &diagnostics,
        .before_read = &before_soldier_read,
        .after_read = &after_soldier_read,
    };
}

} // namespace fusioncutter::patches::hero_diagnostics
