#include "pipeline.hpp"

#include "layout.hpp"

#include <array>
#include <atomic>
#include <cstddef>

namespace fusioncutter::patches::hero_melee_pipeline {
namespace {

using UpdateFunction = bool(__thiscall*)(void*, float);
using SetNetworkStateFunction = void(__thiscall*)(void*, int, bool);
using EnterStateFunction = void(__thiscall*)(void*, int);
using AnimatorStateFunction = void(__thiscall*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                std::uint32_t);
using SoundPlayFunction = void(__thiscall*)(void*, void*, void*, std::uint32_t, std::uint32_t);

struct TransitionFrame {
    void* weapon{};
};

struct TransitionStack {
    [[nodiscard]] bool push(TransitionFrame frame) noexcept {
        if (size == frames.size()) {
            return false;
        }
        frames[size++] = frame;
        return true;
    }

    [[nodiscard]] const TransitionFrame* current() const noexcept {
        return size == 0 ? nullptr : &frames[size - 1];
    }

    void pop() noexcept {
        if (size != 0) {
            --size;
        }
    }

    std::array<TransitionFrame, 4> frames{};
    std::size_t size{};
    std::size_t overflow{};
};

std::atomic<const PolicyCallbacks*> gPolicy;
std::atomic<const DiagnosticsCallbacks*> gDiagnostics;
OriginalFunction<UpdateFunction> gUpdateOriginal;
OriginalFunction<SetNetworkStateFunction> gSetNetworkStateOriginal;
OriginalFunction<EnterStateFunction> gEnterStateOriginal;
OriginalFunction<AnimatorStateFunction> gAnimatorStateOriginal;
OriginalFunction<SoundPlayFunction> gTransitionSoundOriginal;
thread_local TransitionStack gTransitions;

bool __fastcall hook_update(void* weapon, void*, float delta) noexcept {
    const auto* policy = gPolicy.load(std::memory_order_acquire);
    const auto mode =
        policy == nullptr ? MeleeUpdateMode::Native : policy->before_update(policy->context, weapon, delta);
    const auto native_requested = mode == MeleeUpdateMode::Native;
    const auto effective_delta = native_requested ? delta : 0.0F;
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->before_update(diagnostics->context, weapon, delta, effective_delta, mode);
    }

    const auto original = gUpdateOriginal.get();
    const auto native_called = native_requested && original != nullptr;
    const auto result = native_called ? original(weapon, delta) : true;

    if (diagnostics != nullptr) {
        diagnostics->after_update(diagnostics->context, weapon, delta, effective_delta, native_called, result);
    }
    if (policy != nullptr) {
        policy->after_update(policy->context, weapon, delta, native_called, result);
    }
    return result;
}

void __fastcall hook_set_network_state(void* weapon, void*, int state, bool flag) noexcept {
    const auto* policy = gPolicy.load(std::memory_order_acquire);
    const auto mode = policy == nullptr ? NetworkStateMode::Native
                                        : policy->before_network_state(policy->context, weapon, state, flag);
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->before_network_state(diagnostics->context, weapon, state, flag, mode);
    }

    const auto original = gSetNetworkStateOriginal.get();
    const auto native_called = mode == NetworkStateMode::Native && original != nullptr;
    if (native_called) {
        original(weapon, state, flag);
    }

    if (diagnostics != nullptr) {
        diagnostics->after_network_state(diagnostics->context, weapon, state, flag, native_called);
    }
    if (policy != nullptr) {
        policy->after_network_state(policy->context, weapon, state, flag, native_called);
    }
}

void __fastcall hook_enter_state(void* weapon, void*, int state) noexcept {
    const auto* policy = gPolicy.load(std::memory_order_acquire);
    if (policy != nullptr) {
        policy->enter_state(policy->context, weapon, state);
    }
    const auto tracked = gTransitions.push({weapon});
    if (!tracked) {
        ++gTransitions.overflow;
    }
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->before_enter_state(diagnostics->context, weapon, state);
    }
    if (const auto original = gEnterStateOriginal.get(); original != nullptr) {
        original(weapon, state);
    }
    if (diagnostics != nullptr) {
        diagnostics->after_enter_state(diagnostics->context, weapon, state);
    }
    if (tracked) {
        gTransitions.pop();
    } else {
        --gTransitions.overflow;
    }
}

void __fastcall hook_animator_state(void* animator, void*, std::uint32_t weapon_state, std::uint32_t active,
                                    std::uint32_t primary_animation, std::uint32_t secondary_animation,
                                    std::uint32_t blend) noexcept {
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->before_animator_state(diagnostics->context, animator, weapon_state, active, primary_animation,
                                           secondary_animation);
    }
    if (const auto original = gAnimatorStateOriginal.get(); original != nullptr) {
        original(animator, weapon_state, active, primary_animation, secondary_animation, blend);
    }
    if (diagnostics != nullptr) {
        diagnostics->after_animator_state(diagnostics->context, animator, weapon_state, active, primary_animation,
                                          secondary_animation);
    }
}

void __fastcall hook_transition_sound(void* sound, void*, void* first, void* second, std::uint32_t third,
                                      std::uint32_t fourth) noexcept {
    // This redirected EnterState call leaves every other game sound on its native path.
    const auto* transition = gTransitions.overflow == 0 ? gTransitions.current() : nullptr;
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire); diagnostics != nullptr) {
        diagnostics->transition_sound(diagnostics->context,
                                      {
                                          .weapon = transition == nullptr ? nullptr : transition->weapon,
                                          .sound = sound,
                                          .argument0 = first,
                                          .argument1 = second,
                                          .argument2 = third,
                                          .argument3 = fourth,
                                      });
    }
    if (const auto original = gTransitionSoundOriginal.get(); original != nullptr) {
        original(sound, first, second, third, fourth);
    }
}

void observe_input_queue(MidHookContext& context) noexcept {
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->input_queue(diagnostics->context, context, false);
    }
    if (const auto* policy = gPolicy.load(std::memory_order_acquire); policy != nullptr) {
        policy->input_queue(policy->context, context);
    }
    if (diagnostics != nullptr) {
        diagnostics->input_queue(diagnostics->context, context, true);
    }
}

void observe_prediction_transition(MidHookContext& context) noexcept {
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->prediction_transition(diagnostics->context, context, false);
    }
    if (const auto* policy = gPolicy.load(std::memory_order_acquire); policy != nullptr) {
        policy->prediction_transition(policy->context, context);
    }
    if (diagnostics != nullptr) {
        diagnostics->prediction_transition(diagnostics->context, context, true);
    }
}

} // namespace

HeroMeleePipeline::HeroMeleePipeline(const TargetContext& target) noexcept : target_(target) {}

void HeroMeleePipeline::build_plan(PatchPlan& plan) {
    const auto& layout = layout_for(target_.layout);
    gUpdateOriginal = plan.inline_hook_with_original<UpdateFunction>("Share hero melee updates", layout.update.rva,
                                                                     layout.update.pattern(),
                                                                     reinterpret_cast<UpdateFunction>(&hook_update));
    gSetNetworkStateOriginal = plan.inline_hook_with_original<SetNetworkStateFunction>(
        "Share authoritative hero melee states", layout.set_network_state.rva, layout.set_network_state.pattern(),
        reinterpret_cast<SetNetworkStateFunction>(&hook_set_network_state));
    gEnterStateOriginal = plan.inline_hook_with_original<EnterStateFunction>(
        "Share hero melee transitions", layout.enter_state.rva, layout.enter_state.pattern(),
        reinterpret_cast<EnterStateFunction>(&hook_enter_state));
    gAnimatorStateOriginal = plan.inline_hook_with_original<AnimatorStateFunction>(
        "Share hero melee animator state", layout.animator_state.rva, layout.animator_state.pattern(),
        reinterpret_cast<AnimatorStateFunction>(&hook_animator_state));
    gTransitionSoundOriginal = plan.redirect_call_with_original<SoundPlayFunction>(
        "Share hero melee transition sounds", layout.transition_sound.rva, layout.transition_sound.pattern(),
        reinterpret_cast<SoundPlayFunction>(&hook_transition_sound));
    plan.mid_hook("Share hero melee input edges", layout.input_queue_update.rva,
                  BytePattern::exact(input_queue_preimage(target_.image, layout)), &observe_input_queue);
    plan.mid_hook("Share predicted hero melee transitions", layout.prediction_transition.rva,
                  layout.prediction_transition.pattern(), &observe_prediction_transition);
}

void publish_policy(const PolicyCallbacks& callbacks) noexcept {
    gPolicy.store(&callbacks, std::memory_order_release);
}

void clear_policy(const PolicyCallbacks& callbacks) noexcept {
    auto* expected = &callbacks;
    static_cast<void>(gPolicy.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel));
}

void publish_diagnostics(const DiagnosticsCallbacks& callbacks) noexcept {
    gDiagnostics.store(&callbacks, std::memory_order_release);
}

void clear_diagnostics(const DiagnosticsCallbacks& callbacks) noexcept {
    auto* expected = &callbacks;
    static_cast<void>(gDiagnostics.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel));
}

} // namespace fusioncutter::patches::hero_melee_pipeline
