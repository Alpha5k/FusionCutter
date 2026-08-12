#pragma once

#include "layout.hpp"
#include "smoothing.hpp"

#include <FusionCutter/patch.hpp>

#include <atomic>
#include <cstdint>

namespace fusioncutter::patches::spectator_camera {

// Smooths the spectated player while preserving the game's native spectator camera.
class SpectatorCameraSmoothing final : public RuntimePatch {
  public:
    explicit SpectatorCameraSmoothing(const TargetContext& target) noexcept;

    // Validate spectator state and install the object smoothing and camera confirmation points.
    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using GetLocalIndex = int(__cdecl*)(int connection) noexcept;
    using GetSpectator = int(__cdecl*)(int local_index) noexcept;

    // Maintain one coherent object-motion history for the currently spectated soldier.
    void record_root(void* soldier, const float* matrix) noexcept;
    void record_object_publication(void* soldier, int object_index, const float* matrix) noexcept;
    void smooth_object_render(int object_index, float* matrix) noexcept;
    void record_camera_publication(void* camera) noexcept;
    void deactivate() noexcept;
    [[nodiscard]] bool is_actively_spectating(int& spectator_index) const noexcept;

    // Adapt each reviewed register and stack boundary to the typed smoothing operations above.
    static void observe_root(MidHookContext& context) noexcept;
    static void observe_object_publication(MidHookContext& context) noexcept;
    static void observe_object_render(MidHookContext& context) noexcept;
    static void observe_camera_publication(MidHookContext& context) noexcept;

    const SpectatorCameraLayout& layout_;
    ImageContext image_;
    const volatile std::int32_t* update_turn_{};
    const volatile float* turn_ratio_{};
    const volatile std::uint8_t* network_provider_flag_{};
    const volatile std::uint8_t* spectator_enabled_{};
    const volatile std::uint8_t* alternate_spectator_enabled_{};
    void* const volatile* net_game_global_{};
    GetLocalIndex get_local_index_{};
    GetSpectator get_spectator_{};
    std::atomic<void*> active_soldier_{};
    std::atomic<int> tracked_object_index_{-1};
    TransformSmoother smoother_;
    std::uint32_t owner_thread_id_{};
    int active_spectator_index_{-1};

    inline static PatchInstanceSlot<SpectatorCameraSmoothing> active_;
};

} // namespace fusioncutter::patches::spectator_camera
