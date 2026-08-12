#pragma once

#include <FusionCutter/patching.hpp>

#include <cstdint>

namespace fusioncutter::patches::spectator_camera {

struct SpectatorHookLayout {
    NativeSite<15> root_capture;
    NativeSite<5> object_publication;
    NativeSite<5> object_render_call;
    NativeSite<7> object_render_capture;
    NativeSite<5> camera_publication;
};

struct SpectatorContextLayout {
    NativeSite<13> root_owner;
    NativeSite<8> root_matrix_call;
    NativeSite<7> object_publication_arguments;
    NativeSite<14> object_render_arguments;
    NativeSite<17> camera_publication_arguments;
};

struct SpectatorStateLayout {
    std::uint32_t mode_probe_rva;
    std::uint32_t state_probe_rva;
    std::uint32_t update_turn_access_rva;
    std::uint32_t turn_ratio_getter_rva;
    std::uint32_t update_turn_rva;
    std::uint32_t turn_ratio_rva;
    std::uint32_t network_provider_flag_rva;
    std::uint32_t spectator_enabled_rva;
    std::uint32_t alternate_spectator_enabled_rva;
    std::uint32_t net_game_global_rva;
    std::uint32_t get_local_index_rva;
    std::uint32_t get_spectator_rva;
};

// Describes the hook contexts and native spectator state used by one retail target.
struct SpectatorCameraLayout {
    SpectatorHookLayout hooks;
    SpectatorContextLayout contexts;
    SpectatorStateLayout state;
};

[[nodiscard]] const SpectatorCameraLayout& layout_for(TargetLayout target) noexcept;
// Adds the native ABI and state proofs that are not already owned by a hook operation.
void add_layout_requirements(PatchPlan& plan, const ImageContext& image, const SpectatorCameraLayout& layout);

} // namespace fusioncutter::patches::spectator_camera
