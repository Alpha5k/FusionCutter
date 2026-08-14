#pragma once

#include "patch.hpp"
#include "trace/recorder.hpp"
#include "../../pipelines/network/pipeline.hpp"
#include "../../pipelines/soldier_state/pipeline.hpp"

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::network_diagnostics {

// Owns the role-specific observers and publishes their bounded records to one ETL session.
class NetworkDiagnostics final : public RuntimePatch, public StatusContributor {
  public:
    NetworkDiagnostics(NetworkDiagnosticsSettings settings, const TargetContext& target) noexcept;
    ~NetworkDiagnostics() override;

    void build_plan(PatchPlan& plan) override;
    [[nodiscard]] std::expected<void, OutcomeReason> prepare_runtime() override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;
    void write_status(StatusSection& output) const noexcept override;

    [[nodiscard]] CaptureMode capture_mode() const noexcept;
    [[nodiscard]] trace::Recorder& recorder() noexcept;

    void observe_group(int destination, bool begin) noexcept;
    void observe_send(int destination, std::size_t bytes, network_pipeline::PacketCarrier carrier, int result) noexcept;
    void observe_receive(bool begin) noexcept;
    void observe_intake(void* endpoint, bool direct) noexcept;
    void observe_disconnect(int destination, bool begin) noexcept;
    void observe_reset(std::uint8_t mode) noexcept;
    void observe_direct_association(const network_pipeline::DirectAssociationObservation& observation) noexcept;
    void observe_direct_receive(const network_pipeline::DirectReceiveObservation& observation) noexcept;
    void observe_output_pacing(const network_pipeline::OutputPacingObservation& observation) noexcept;

  private:
    NetworkDiagnosticsSettings settings_;
    TargetContext target_;
    trace::Recorder recorder_;
    network_pipeline::DiagnosticsCallbacks pipeline_callbacks_;
    soldier_state_pipeline::ObserverCallbacks soldier_state_observer_{};
};

} // namespace fusioncutter::patches::network_diagnostics
