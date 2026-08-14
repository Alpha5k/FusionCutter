#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::hero_melee_pipeline {

struct UpdateDecision {
    bool call_native{true};
    bool result{true};
};

struct NetworkStateDecision {
    int state;
    bool call_native{true};
};

// Supplies the one gameplay policy allowed to alter shared hero melee boundaries.
struct PolicyCallbacks {
    void* context;
    UpdateDecision (*before_update)(void*, void*, float) noexcept;
    void (*after_update)(void*, void*, float, bool, bool) noexcept;
    NetworkStateDecision (*before_network_state)(void*, void*, int, bool) noexcept;
    void (*after_network_state)(void*, void*, int, bool, bool) noexcept;
    void (*enter_state)(void*, void*, int) noexcept;
    bool (*suppress_animator_state)(void*, void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) noexcept;
    void (*input_queue)(void*, MidHookContext&) noexcept;
    void (*prediction_transition)(void*, MidHookContext&) noexcept;
};

// Supplies the fixed observations emitted around the shared policy and native calls.
struct DiagnosticsCallbacks {
    void* context;
    void (*before_update)(void*, void*, float, const UpdateDecision&) noexcept;
    void (*after_update)(void*, void*, float, bool, bool) noexcept;
    void (*before_network_state)(void*, void*, int, bool, const NetworkStateDecision&) noexcept;
    void (*after_network_state)(void*, void*, int, bool, bool) noexcept;
    void (*before_enter_state)(void*, void*, int) noexcept;
    void (*after_enter_state)(void*, void*, int) noexcept;
    void (*before_animator_state)(void*, void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) noexcept;
    void (*after_animator_state)(void*, void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                 bool) noexcept;
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
