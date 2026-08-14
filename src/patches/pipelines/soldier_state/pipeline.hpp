#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::soldier_state_pipeline {

struct ReadContext {
    void* soldier;
    void* stream;
    std::uint32_t flags;
};

struct ObserverState {
    std::uint64_t first{};
    std::uint64_t second{};
};

// Receives paired observations around the shared Soldier state reader.
struct ObserverCallbacks {
    void* context;
    void (*before_read)(void*, const ReadContext&, ObserverState&) noexcept;
    void (*after_read)(void*, const ReadContext&, const ObserverState&) noexcept;
};

void publish_observer(const ObserverCallbacks& callbacks) noexcept;
void clear_observer(const ObserverCallbacks& callbacks) noexcept;

class SoldierStatePipeline final : public Patch {
  public:
    explicit SoldierStatePipeline(const TargetContext& target) noexcept;

    // Installs the Soldier state boundary shared by diagnostics consumers.
    void build_plan(PatchPlan& plan) override;

  private:
    TargetContext target_;
};

} // namespace fusioncutter::patches::soldier_state_pipeline
