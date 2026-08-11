#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::door_corpse_fix {

class DoorCorpseFix final : public RuntimePatch {
  public:
    explicit DoorCorpseFix(const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using ObjectQuery = int(__cdecl*)(const float* center, float radius, void** objects, int capacity, void* teams,
                                      int affiliation) noexcept;

    [[nodiscard]] bool is_dead_soldier(const void* object) const noexcept;
    [[nodiscard]] int query(const float* center, float radius, void** objects, int capacity, void* teams,
                            int affiliation) const noexcept;
    [[nodiscard]] static int __cdecl query_hook(const float* center, float radius, void** objects, int capacity,
                                                void* teams, int affiliation) noexcept;

    TargetLayout layout_;
    const void* soldier_rtti_getter_{};

    inline static PatchInstanceSlot<DoorCorpseFix> active_;
    inline static OriginalFunction<ObjectQuery> original_query_;
};

} // namespace fusioncutter::patches::door_corpse_fix
