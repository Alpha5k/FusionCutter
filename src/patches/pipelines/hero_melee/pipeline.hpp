#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::hero_melee_pipeline {

// Selects the one native owner of hero melee state progression.
enum class MeleeUpdateMode {
    Native,
    SkipRedundantAuthority,
};

// Selects whether one authoritative state/flag pair is current or historical.
enum class NetworkStateMode {
    Native,
    SuppressHistorical,
};

// Carries the exact EnterState sound call without exposing the global game-sound boundary.
struct TransitionSound {
    void* weapon;
    void* sound;
    void* argument0;
    void* argument1;
    std::uint32_t argument2;
    std::uint32_t argument3;
};

// Supplies the one gameplay policy allowed to alter shared hero melee boundaries.
struct PolicyCallbacks {
    void* context;
    MeleeUpdateMode (*before_update)(void*, void*, float) noexcept;
    void (*after_update)(void*, void*, float, bool, bool) noexcept;
    NetworkStateMode (*before_network_state)(void*, void*, int, bool) noexcept;
    void (*after_network_state)(void*, void*, int, bool, bool) noexcept;
    void (*enter_state)(void*, void*, int) noexcept;
    void (*input_queue)(void*, MidHookContext&) noexcept;
    void (*prediction_transition)(void*, MidHookContext&) noexcept;
};

// Supplies the fixed observations emitted around the shared policy and native calls.
struct DiagnosticsCallbacks {
    void* context;
    void (*before_update)(void*, void*, float, float, MeleeUpdateMode) noexcept;
    void (*after_update)(void*, void*, float, float, bool, bool) noexcept;
    void (*before_network_state)(void*, void*, int, bool, NetworkStateMode) noexcept;
    void (*after_network_state)(void*, void*, int, bool, bool) noexcept;
    void (*before_enter_state)(void*, void*, int) noexcept;
    void (*after_enter_state)(void*, void*, int) noexcept;
    void (*before_animator_state)(void*, void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) noexcept;
    void (*after_animator_state)(void*, void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) noexcept;
    void (*transition_sound)(void*, const TransitionSound&) noexcept;
    void (*input_queue)(void*, const MidHookContext&, bool) noexcept;
    void (*prediction_transition)(void*, const MidHookContext&, bool) noexcept;
};

void publish_policy(const PolicyCallbacks& callbacks) noexcept;
void clear_policy(const PolicyCallbacks& callbacks) noexcept;
void publish_diagnostics(const DiagnosticsCallbacks& callbacks) noexcept;
void clear_diagnostics(const DiagnosticsCallbacks& callbacks) noexcept;

class HeroMeleePipeline final : public Patch {
  public:
    explicit HeroMeleePipeline(const TargetContext& target) noexcept;

    // Installs the native hero boundaries shared by reconciliation and diagnostics.
    void build_plan(PatchPlan& plan) override;

  private:
    TargetContext target_;
};

} // namespace fusioncutter::patches::hero_melee_pipeline
