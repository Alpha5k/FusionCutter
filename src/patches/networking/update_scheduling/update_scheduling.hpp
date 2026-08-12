#pragma once

#include <FusionCutter/patch.hpp>

#include <array>

namespace fusioncutter::patches::update_scheduling {

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

    // Reimplements bf2_create_fence_cc's capture of the pending map, turn, and send time.
    void record_create(std::uint32_t player) noexcept;

    // Reimplements bf2_create_fence_blocks, including its acknowledgement timeout and native NACK reset.
    [[nodiscard]] bool fence_blocks(std::uint32_t player) noexcept;

    // Reimplements bf2_su2_slotfix_cc's guard around SentUpdate's two acknowledgement slots.
    static void guard_sent_slot(MidHookContext& context) noexcept;

    // Reimplements bf2_create_fence_cc's WriteObjects hook without its manual assembly trampoline.
    static void capture_create(MidHookContext& context) noexcept;

    // Reimplements bf2_create_fence_gate_cc while preserving the earlier IsPipeFull gate.
    static void gate_destination(MidHookContext& context) noexcept;
};

} // namespace fusioncutter::patches::update_scheduling
