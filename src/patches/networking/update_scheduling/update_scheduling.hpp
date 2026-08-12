#pragma once

#include <FusionCutter/patch.hpp>

#include <array>

namespace fusioncutter::patches::update_scheduling {

// Enables per-turn client updates while fencing each native object-map CREATE transaction until settlement.
class UpdateScheduling final : public RuntimePatch {
  public:
    explicit UpdateScheduling(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    static constexpr std::size_t kMaximumPlayers = 64;

    struct CreateFence {
        void* pending_map;
        std::int32_t pending_turn;
        float sent_time;
    };

    using AllocateObjectMap = void*(__cdecl*)();
    using FreeObjectMap = void(__thiscall*)(void*);

    ImageContext image_;
    std::array<CreateFence, kMaximumPlayers> fences_{};
    AllocateObjectMap allocate_object_map_;
    FreeObjectMap free_object_map_;

    [[nodiscard]] std::byte* player_state(std::size_t player) const noexcept;
    void record_create(std::uint32_t player) noexcept;
    [[nodiscard]] bool fence_blocks(std::uint32_t player) noexcept;

    static void guard_sent_slot(MidHookContext& context) noexcept;
    static void capture_create(MidHookContext& context) noexcept;
    static void gate_destination(MidHookContext& context) noexcept;
};

} // namespace fusioncutter::patches::update_scheduling
