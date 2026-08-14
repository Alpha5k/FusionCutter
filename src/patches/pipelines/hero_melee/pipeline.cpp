#include "pipeline.hpp"

#include "layout.hpp"

#include <atomic>

namespace fusioncutter::patches::hero_melee_pipeline {
namespace {

using UpdateFunction = bool(__thiscall*)(void*, float);
using SetNetworkStateFunction = void(__thiscall*)(void*, int, bool);
using EnterStateFunction = void(__thiscall*)(void*, int);
using AnimatorStateFunction = void(__thiscall*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                std::uint32_t);

std::atomic<const PolicyCallbacks*> gPolicy;
std::atomic<const DiagnosticsCallbacks*> gDiagnostics;
OriginalFunction<UpdateFunction> gUpdateOriginal;
OriginalFunction<SetNetworkStateFunction> gSetNetworkStateOriginal;
OriginalFunction<EnterStateFunction> gEnterStateOriginal;
OriginalFunction<AnimatorStateFunction> gAnimatorStateOriginal;

bool __fastcall hook_update(void* weapon, void*, float delta) noexcept {
    const auto* policy = gPolicy.load(std::memory_order_acquire);
    const auto decision = policy == nullptr ? UpdateDecision{} : policy->before_update(policy->context, weapon, delta);
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->before_update(diagnostics->context, weapon, delta, decision);
    }

    const auto original = gUpdateOriginal.get();
    const auto result = decision.call_native && original != nullptr ? original(weapon, delta) : decision.result;

    if (diagnostics != nullptr) {
        diagnostics->after_update(diagnostics->context, weapon, delta, decision.call_native, result);
    }
    if (policy != nullptr) {
        policy->after_update(policy->context, weapon, delta, decision.call_native, result);
    }
    return result;
}

void __fastcall hook_set_network_state(void* weapon, void*, int state, bool flag) noexcept {
    const auto* policy = gPolicy.load(std::memory_order_acquire);
    const auto decision = policy == nullptr ? NetworkStateDecision{state}
                                            : policy->before_network_state(policy->context, weapon, state, flag);
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->before_network_state(diagnostics->context, weapon, state, flag, decision);
    }

    if (decision.call_native) {
        if (const auto original = gSetNetworkStateOriginal.get(); original != nullptr) {
            original(weapon, decision.state, flag);
        }
    }

    if (diagnostics != nullptr) {
        diagnostics->after_network_state(diagnostics->context, weapon, decision.state, flag, decision.call_native);
    }
    if (policy != nullptr) {
        policy->after_network_state(policy->context, weapon, decision.state, flag, decision.call_native);
    }
}

void __fastcall hook_enter_state(void* weapon, void*, int state) noexcept {
    const auto* policy = gPolicy.load(std::memory_order_acquire);
    if (policy != nullptr) {
        policy->enter_state(policy->context, weapon, state);
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
}

void __fastcall hook_animator_state(void* animator, void*, std::uint32_t weapon_state, std::uint32_t active,
                                    std::uint32_t primary_animation, std::uint32_t secondary_animation,
                                    std::uint32_t blend) noexcept {
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->before_animator_state(diagnostics->context, animator, weapon_state, active, primary_animation,
                                           secondary_animation);
    }
    const auto* policy = gPolicy.load(std::memory_order_acquire);
    const auto suppressed =
        policy != nullptr && policy->suppress_animator_state(policy->context, animator, weapon_state, active,
                                                             primary_animation, secondary_animation);
    if (!suppressed) {
        if (const auto original = gAnimatorStateOriginal.get(); original != nullptr) {
            original(animator, weapon_state, active, primary_animation, secondary_animation, blend);
        }
    }
    if (diagnostics != nullptr) {
        diagnostics->after_animator_state(diagnostics->context, animator, weapon_state, active, primary_animation,
                                          secondary_animation, suppressed);
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
