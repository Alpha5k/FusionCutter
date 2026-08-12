#pragma once

#include "layout.hpp"

#include <FusionCutter/patch.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::update_recovery {

// Replaces the client's newest-only receive drain with ordered processing of every complete update.
class LateUpdateRecovery final : public RuntimePatch {
  public:
    explicit LateUpdateRecovery(const TargetContext& target) noexcept;

    // Validates the ReceiveClient frame contract and installs the replacement drain callback.
    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using ConstructGroup = void*(__fastcall*)(void* group) noexcept;
    using ReceiveGroup = bool(__fastcall*)(void* group, int packet_type) noexcept;
    using GetUpdateTurn = std::uint32_t(__cdecl*)(void* group) noexcept;
    using ReceiveUpdate = void(__cdecl*)(void* group) noexcept;
    using GetLastReceive = float*(__fastcall*)(int pipe_index) noexcept;

    static constexpr std::size_t kNativeGroupBytes = 0x1C;
    static constexpr std::size_t kMaximumBatchGroups = 512;

    struct alignas(4) BufferedUpdate {
        std::array<std::byte, kNativeGroupBytes> group{};
        std::uint32_t turn{};
    };
    static_assert(sizeof(BufferedUpdate) == 0x20);

    // Enters the replacement drain from the verified ReceiveClient stack frame, then skips the stock drain.
    static void recover_complete_updates(MidHookContext& context) noexcept;
    // Receives complete update groups in fixed storage and submits all of them through the native decoder.
    void drain(float receive_timestamp, int pipe_index_base) noexcept;
    // Reproduces the receive timestamp and host-cycle timeout updates performed by the replaced block.
    void refresh_receive_timers(float receive_timestamp, int pipe_index_base) const noexcept;

    const UpdateRecoveryLayout& layout_;
    ImageContext image_;
    ConstructGroup construct_group_{};
    ReceiveGroup receive_group_{};
    GetUpdateTurn get_update_turn_{};
    ReceiveUpdate receive_update_{};
    GetLastReceive get_last_receive_{};
    float* host_cycle_timer_{};
    const float* host_cycle_extension_{};
    std::uintptr_t resume_address_{};
    std::array<BufferedUpdate, kMaximumBatchGroups> updates_{};

    inline static PatchInstanceSlot<LateUpdateRecovery> active_;
};

} // namespace fusioncutter::patches::update_recovery
