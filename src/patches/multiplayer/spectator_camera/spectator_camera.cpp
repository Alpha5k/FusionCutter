#include "spectator_camera.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::spectator_camera {
namespace {

constexpr std::size_t kNetGameConnectionOffset = 0x50;
constexpr std::size_t kSpectatorObjectIndexOffset = 0x21C;
// The root hook receives EntitySoldier's embedded controllable subobject.
constexpr std::size_t kSpectatorControllableOffset = 0x258;
// Camera publication exposes the adjusted target and completed RedCamera matrix at these fields.
constexpr std::size_t kCameraAdjustedTargetOffset = 0x0C;
constexpr std::size_t kRedCameraMatrixOffset = 0x30;
constexpr std::size_t kRenderedObjectIndexOffset = 0x188;
constexpr std::size_t kObjectMatrixLocalOffset = 0xA0;
constexpr int kMaximumObjectIndex = 64;

[[nodiscard]] bool valid_object_index(int index) noexcept {
    return index >= 0 && index < kMaximumObjectIndex;
}

} // namespace

SpectatorCameraSmoothing::SpectatorCameraSmoothing(const TargetContext& target) noexcept
    : layout_(layout_for(target.layout)), image_(target.image),
      update_turn_(image_.read_at_rva<volatile std::int32_t>(layout_.state.update_turn_rva)),
      turn_ratio_(image_.read_at_rva<volatile float>(layout_.state.turn_ratio_rva)),
      network_provider_flag_(image_.read_at_rva<volatile std::uint8_t>(layout_.state.network_provider_flag_rva)),
      spectator_enabled_(image_.read_at_rva<volatile std::uint8_t>(layout_.state.spectator_enabled_rva)),
      alternate_spectator_enabled_(
          image_.read_at_rva<volatile std::uint8_t>(layout_.state.alternate_spectator_enabled_rva)),
      net_game_global_(reinterpret_cast<void* const volatile*>(
          image_.address_at_rva(layout_.state.net_game_global_rva, sizeof(void*)))),
      get_local_index_(image_.function_at_rva<GetLocalIndex>(layout_.state.get_local_index_rva)),
      get_spectator_(image_.function_at_rva<GetSpectator>(layout_.state.get_spectator_rva)) {}

void SpectatorCameraSmoothing::build_plan(PatchPlan& plan) {
    add_layout_requirements(plan, image_, layout_);
    plan.mid_hook("Observe the spectated soldier", layout_.hooks.root_capture.rva, layout_.hooks.root_capture.pattern(),
                  &SpectatorCameraSmoothing::observe_root);
    plan.mid_hook("Record spectated object updates", layout_.hooks.object_publication.rva,
                  layout_.hooks.object_publication.pattern(), &SpectatorCameraSmoothing::observe_object_publication);
    plan.mid_hook("Smooth the rendered spectator object", layout_.hooks.object_render_capture.rva,
                  layout_.hooks.object_render_capture.pattern(), &SpectatorCameraSmoothing::observe_object_render);
    plan.mid_hook("Record spectator camera updates", layout_.hooks.camera_publication.rva,
                  layout_.hooks.camera_publication.pattern(), &SpectatorCameraSmoothing::observe_camera_publication);
    plan.mid_hook("Smooth the rendered spectator camera", layout_.hooks.camera_render.rva,
                  layout_.hooks.camera_render.pattern(), &SpectatorCameraSmoothing::observe_camera_render);
}

void SpectatorCameraSmoothing::enable_runtime() noexcept {
    deactivate();
    active_.publish(*this);
}

void SpectatorCameraSmoothing::disable_runtime() noexcept {
    active_.clear(*this);
    deactivate();
}

void SpectatorCameraSmoothing::record_root(void* soldier, const float* matrix) noexcept {
    int spectator_index{};
    if (soldier == nullptr || matrix == nullptr || !is_actively_spectating(spectator_index)) {
        deactivate();
        return;
    }

    const auto thread_id = GetCurrentThreadId();
    const int object_index = read_native_field<int>(soldier, kSpectatorObjectIndexOffset);
    if (!valid_object_index(object_index)) {
        deactivate();
        return;
    }

    const auto* previous = active_soldier_.load(std::memory_order_acquire);
    if (previous != soldier || active_spectator_index_ != spectator_index ||
        (owner_thread_id_ != 0 && owner_thread_id_ != thread_id)) {
        smoother_.reset();
        active_interpolator_.store(nullptr, std::memory_order_release);
        active_soldier_.store(soldier, std::memory_order_release);
        owner_thread_id_ = thread_id;
    }

    active_spectator_index_ = spectator_index;
    tracked_object_index_.store(object_index, std::memory_order_release);
}

void SpectatorCameraSmoothing::record_object_publication(void* soldier, int object_index,
                                                         const float* matrix) noexcept {
    if (soldier == nullptr || matrix == nullptr || !valid_object_index(object_index) ||
        object_index != tracked_object_index_.load(std::memory_order_acquire) ||
        active_soldier_.load(std::memory_order_acquire) != soldier || owner_thread_id_ != GetCurrentThreadId()) {
        return;
    }
    static_cast<void>(smoother_.publish_object(soldier, *update_turn_, matrix));
}

void SpectatorCameraSmoothing::smooth_object_render(int object_index, float* matrix) noexcept {
    if (matrix == nullptr || !valid_object_index(object_index) ||
        object_index != tracked_object_index_.load(std::memory_order_acquire) ||
        owner_thread_id_ != GetCurrentThreadId()) {
        return;
    }

    if (active_soldier_.load(std::memory_order_acquire) == nullptr) {
        return;
    }
    if (smoother_.smooth(TransformPath::Object, *turn_ratio_, matrix) == SmoothingResult::PhaseMismatch) {
        smoother_.reset();
        active_interpolator_.store(nullptr, std::memory_order_release);
    }
}

void SpectatorCameraSmoothing::record_camera_publication(void* interpolator, void* camera, void* red_camera) noexcept {
    if (interpolator == nullptr || camera == nullptr || red_camera == nullptr) {
        return;
    }

    void* const soldier = active_soldier_.load(std::memory_order_acquire);
    void* const adjusted_target = read_native_field<void*>(camera, kCameraAdjustedTargetOffset);
    void* const expected_target =
        soldier == nullptr ? nullptr : static_cast<std::byte*>(soldier) + kSpectatorControllableOffset;
    if (soldier == nullptr || adjusted_target != expected_target || owner_thread_id_ != GetCurrentThreadId()) {
        return;
    }

    const auto* matrix =
        reinterpret_cast<const float*>(static_cast<const std::byte*>(red_camera) + kRedCameraMatrixOffset);
    active_interpolator_.store(interpolator, std::memory_order_release);
    static_cast<void>(smoother_.publish_camera(soldier, *update_turn_, matrix));
}

void SpectatorCameraSmoothing::smooth_camera_render(void* interpolator, float* matrix) noexcept {
    void* const soldier = active_soldier_.load(std::memory_order_acquire);
    if (interpolator == nullptr || matrix == nullptr ||
        active_interpolator_.load(std::memory_order_acquire) != interpolator ||
        owner_thread_id_ != GetCurrentThreadId() || !smoother_.ready(soldier)) {
        return;
    }

    if (smoother_.smooth(TransformPath::Camera, *turn_ratio_, matrix) == SmoothingResult::PhaseMismatch) {
        smoother_.reset();
        active_interpolator_.store(nullptr, std::memory_order_release);
    }
}

void SpectatorCameraSmoothing::deactivate() noexcept {
    if (tracked_object_index_.load(std::memory_order_acquire) == -1) {
        return;
    }
    active_soldier_.store(nullptr, std::memory_order_release);
    active_interpolator_.store(nullptr, std::memory_order_release);
    tracked_object_index_.store(-1, std::memory_order_release);
    smoother_.reset();
    owner_thread_id_ = 0;
    active_spectator_index_ = -1;
}

bool SpectatorCameraSmoothing::is_actively_spectating(int& spectator_index) const noexcept {
    spectator_index = -1;
    const bool spectator_mode =
        *network_provider_flag_ != 0 ? *alternate_spectator_enabled_ != 0 : *spectator_enabled_ != 0;
    if (!spectator_mode) {
        return false;
    }

    const auto* net_game = static_cast<const std::byte*>(*net_game_global_);
    if (net_game == nullptr) {
        return false;
    }

    const int connection = read_native_field<int>(net_game, kNetGameConnectionOffset);
    spectator_index = get_spectator_(get_local_index_(connection));
    return valid_object_index(spectator_index);
}

void SpectatorCameraSmoothing::observe_root(MidHookContext& context) noexcept {
    if (auto* patch = active_.read(); patch != nullptr) {
        auto* soldier = context.esi < kSpectatorControllableOffset
                            ? nullptr
                            : reinterpret_cast<void*>(context.esi - kSpectatorControllableOffset);
        patch->record_root(soldier, reinterpret_cast<const float*>(context.eax));
    }
}

void SpectatorCameraSmoothing::observe_object_publication(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.esp == 0) {
        return;
    }

    const auto* stack = reinterpret_cast<const void*>(context.esp);
    patch->record_object_publication(reinterpret_cast<void*>(context.edi), read_native_field<int>(stack),
                                     read_native_field<const float*>(stack, sizeof(void*)));
}

void SpectatorCameraSmoothing::observe_object_render(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.edi == 0 || context.esp == 0) {
        return;
    }

    patch->smooth_object_render(
        read_native_field<int>(reinterpret_cast<const void*>(context.edi), kRenderedObjectIndexOffset),
        reinterpret_cast<float*>(context.esp + kObjectMatrixLocalOffset));
}

void SpectatorCameraSmoothing::observe_camera_publication(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.esp == 0) {
        return;
    }

    const auto* stack = reinterpret_cast<const void*>(context.esp);
    patch->record_camera_publication(reinterpret_cast<void*>(context.ecx), read_native_field<void*>(stack),
                                     read_native_field<void*>(stack, sizeof(void*)));
}

void SpectatorCameraSmoothing::observe_camera_render(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.esp == 0) {
        return;
    }

    patch->smooth_camera_render(reinterpret_cast<void*>(context.esi),
                                read_native_field<float*>(reinterpret_cast<const void*>(context.esp)));
}

} // namespace fusioncutter::patches::spectator_camera
