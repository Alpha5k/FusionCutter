#pragma once

#include "layout.hpp"
#include "patch.hpp"
#include "schema.hpp"
#include "subject.hpp"
#include "../../pipelines/hero_melee/pipeline.hpp"
#include "../../pipelines/soldier_state/pipeline.hpp"

#include <FusionCutter/diagnostics.hpp>
#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::hero_diagnostics {

class HeroDiagnostics final : public RuntimePatch, public StatusContributor {
  public:
    HeroDiagnostics(HeroDiagnosticsSettings settings, const TargetContext& target) noexcept;
    ~HeroDiagnostics() override;

    void build_plan(PatchPlan& plan) override;
    [[nodiscard]] std::expected<void, OutcomeReason> prepare_runtime() override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;
    void write_status(StatusSection& output) const noexcept override;

    [[nodiscard]] CaptureMode capture_mode() const noexcept;
    [[nodiscard]] HeroSubject* bind(void* weapon) noexcept;
    [[nodiscard]] HeroSubject* find_weapon(const void* weapon) noexcept;
    [[nodiscard]] HeroSubject* find_player(int player) noexcept;
    [[nodiscard]] HeroSubject* find_animator(const void* animator) noexcept;
    [[nodiscard]] HeroSubject* find_soldier(const void* soldier) noexcept;
    [[nodiscard]] trace::TurnContext turn_context() const noexcept;
    [[nodiscard]] std::uint16_t transition_flags(const void* weapon, const trace::TurnContext& turn) const noexcept;
    [[nodiscard]] std::uint32_t state_fingerprint(const HeroSubject& subject) const noexcept;
    [[nodiscard]] trace::LocomotionState locomotion_state(const void* soldier) const noexcept;
    void announce(HeroSubject& subject) noexcept;
    // Guarantees that a subject declaration precedes every record that references it.
    void submit(trace::RecordKind kind, std::span<const std::byte> payload, HeroSubject& subject,
                std::uint16_t flags = trace::RecordFlags::None) noexcept;
    [[nodiscard]] std::uint32_t begin_read_scope() noexcept;
    void end_read_scope(std::uint32_t previous) noexcept;
    [[nodiscard]] std::uint32_t current_read_scope() const noexcept;
    [[nodiscard]] std::uint32_t next_scope() noexcept;
    // Receives the shared melee boundaries without owning duplicate detours.
    void begin_update(void* weapon, float requested_delta, float effective_delta,
                      hero_melee_pipeline::MeleeUpdateMode mode) noexcept;
    void finish_update(void* weapon, float requested_delta, float effective_delta, bool native_called,
                       bool result) noexcept;
    void begin_network_state(void* weapon, int state, bool flag, hero_melee_pipeline::NetworkStateMode mode) noexcept;
    void finish_network_state(void* weapon, int state, bool flag, bool native_called) noexcept;
    void begin_enter_state(void* weapon, int state) noexcept;
    void finish_enter_state(void* weapon, int state) noexcept;
    void begin_animator_state(void* animator, std::uint32_t state, std::uint32_t active, std::uint32_t primary,
                              std::uint32_t secondary) noexcept;
    void finish_animator_state(void* animator, std::uint32_t state, std::uint32_t active, std::uint32_t primary,
                               std::uint32_t secondary) noexcept;
    void observe_transition_sound(const hero_melee_pipeline::TransitionSound& sound) noexcept;
    void observe_input_queue(const MidHookContext& context, bool after) noexcept;
    void observe_prediction_transition(const MidHookContext& context, bool after) noexcept;
    void observe_locomotion(HeroSubject& subject) noexcept;
    void record_locomotion(HeroSubject& subject, trace::LocomotionOperation operation,
                           const trace::LocomotionState& before, const trace::LocomotionState& after,
                           bool result) noexcept;

  private:
    using GetJoystickIndex = SubjectTable::GetJoystickIndex;

    HeroDiagnosticsSettings settings_;
    TargetContext target_;
    const ClientLayout* client_layout_{};
    const ServerLayout* server_layout_{};
    SubjectTable subjects_;
    diagnostics::EtlChannel channel_;
    hero_melee_pipeline::DiagnosticsCallbacks pipeline_callbacks_{};
    soldier_state_pipeline::ObserverCallbacks soldier_state_observer_{};
    const volatile std::int32_t* host_turn_{};
    const volatile std::int32_t* client_turn_{};
    const volatile std::int32_t* update_turn_{};
    const volatile std::int32_t* predict_turn_{};
    const volatile std::int32_t* acknowledged_turn_{};
    const volatile std::int32_t* destination_{};
    const volatile std::uint8_t* is_local_turn_{};
    const volatile std::uint8_t* is_update_turn_{};
    const volatile std::uint8_t* rollback_{};
    const volatile std::uint8_t* network_enabled_{};
    const volatile std::uint8_t* network_client_active_{};
    const std::byte* remote_moves_{};
};

void build_client_plan(PatchPlan& plan, const TargetContext& target, CaptureMode mode);
void build_server_plan(PatchPlan& plan, const TargetContext& target, CaptureMode mode);
[[nodiscard]] soldier_state_pipeline::ObserverCallbacks
make_soldier_state_observer(HeroDiagnostics& diagnostics) noexcept;
void publish_observers(HeroDiagnostics& diagnostics) noexcept;
void clear_observers(HeroDiagnostics& diagnostics) noexcept;
[[nodiscard]] HeroDiagnostics* active_diagnostics() noexcept;

} // namespace fusioncutter::patches::hero_diagnostics
