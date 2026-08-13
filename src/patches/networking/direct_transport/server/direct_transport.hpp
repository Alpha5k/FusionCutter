#pragma once

#include "transport.hpp"
#include "../patch.hpp"
#include "../shared/lobby_hooks.hpp"

#include <FusionCutter/patch.hpp>

#include <optional>

namespace fusioncutter::patches::direct_transport::server {

// Resolves server policy, then owns the server transport and its two callback publications.
class DirectTransportServer final : public RuntimePatch, public StatusContributor {
  public:
    DirectTransportServer(DirectTransportSettings settings, const TargetContext& target);
    ~DirectTransportServer() override;

    // Installs the game hooks and validates the native player-removal operations used by server policy.
    void build_plan(PatchPlan& plan) override;
    // Applies the configured policy and prepares the server's bound UDP transport.
    [[nodiscard]] std::expected<void, OutcomeReason> prepare_runtime() override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;
    void write_status(StatusSection& output) const noexcept override;

  private:
    const GameLayout& layout_;
    Policy policy_;
    std::optional<OutcomeReason> policy_error_;
    ServerTransport transport_;
    LobbyHooks lobby_hooks_;
    network_pipeline::TransportCallbacks pipeline_callbacks_;
    bool enabled_{};
};

} // namespace fusioncutter::patches::direct_transport::server
