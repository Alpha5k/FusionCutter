#pragma once

#include <FusionCutter/patching.hpp>

#include <array>
#include <cstdint>

namespace fusioncutter::patches::update_recovery {

struct ReceiveFrameLayout {
    NativeSite<22> pipe_input;
    std::uint32_t pipe_table_rva;
    NativeSite<10> timestamp;
    NativeSite<18> drain;
    NativeSite<15> resume;
};

struct NativeFunctionsLayout {
    NativeSite<6> construct_group;
    // Prove callable helpers beyond their entry hooks so compatible observers can coexist.
    std::uint32_t receive_group_entry_rva;
    NativeSite<17> receive_group_proof;
    std::uint32_t get_update_turn_entry_rva;
    NativeSite<15> get_update_turn_proof;
    NativeSite<6> receive_update;
    NativeSite<12> get_last_receive;
    std::uint32_t last_receive_table_rva;
};

struct ReceiveStateLayout {
    NativeSite<21> stale_guard;
    std::uint32_t client_host_turn_rva;
    NativeSite<41> timer_refresh;
    std::uint32_t host_cycle_extension_rva;
    std::uint32_t host_cycle_timer_rva;
};

// Describes the ReceiveClient frame, native helpers, and receive state used by one retail target.
struct UpdateRecoveryLayout {
    ReceiveFrameLayout frame;
    NativeFunctionsLayout functions;
    ReceiveStateLayout state;
};

[[nodiscard]] const UpdateRecoveryLayout& layout_for(TargetLayout target) noexcept;
// Adds every native-layout proof except the drain site installed by the owning patch.
void add_layout_requirements(PatchPlan& plan, const ImageContext& image, const UpdateRecoveryLayout& layout);
[[nodiscard]] std::array<std::byte, 18> drain_preimage(const ImageContext& image,
                                                       const UpdateRecoveryLayout& layout) noexcept;

} // namespace fusioncutter::patches::update_recovery
