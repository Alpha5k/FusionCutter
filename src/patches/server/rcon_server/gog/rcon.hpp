#pragma once

#include <FusionCutter/patch.hpp>

#include <memory>

namespace fusioncutter::patches::rcon_server {

// Adapts the game bridge and network service to the framework patch lifecycle.
class GogRcon final : public RuntimePatch {
  public:
    explicit GogRcon(const TargetContext& target);
    ~GogRcon() override;

    void build_plan(PatchPlan& plan) override;
    [[nodiscard]] std::expected<void, OutcomeReason> prepare_runtime() override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace fusioncutter::patches::rcon_server
