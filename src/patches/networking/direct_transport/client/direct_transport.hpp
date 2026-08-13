#pragma once

#include "../shared/lobby_hooks.hpp"
#include "transport.hpp"

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::direct_transport::client {

// Adapts the client transport and game hooks to the framework lifecycle.
class DirectTransportClient final : public RuntimePatch, public StatusContributor {
  public:
    explicit DirectTransportClient(const TargetContext& target) noexcept;
    ~DirectTransportClient() override;

    // Installs the native packet and lobby hooks used by the client transport.
    void build_plan(PatchPlan& plan) override;
    // Opens the client transport before the native hooks become active.
    [[nodiscard]] std::expected<void, OutcomeReason> prepare_runtime() override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;
    void write_status(StatusSection& output) const noexcept override;

  private:
    const GameLayout& layout_;
    ClientTransport transport_;
    LobbyHooks lobby_hooks_;
    network_pipeline::TransportCallbacks pipeline_callbacks_;
};

} // namespace fusioncutter::patches::direct_transport::client
