#pragma once

#include "policy.hpp"
#include "../direct_transport/shared/native_packet.hpp"
#include "../direct_transport/shared/protocol.hpp"
#include "../direct_transport/server/output_pacing.hpp"
#include "../../pipelines/network/pipeline.hpp"

#include <FusionCutter/patch.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace fusioncutter::patches::update_pacing {

// Holds early complete updates until the next server network-service opportunity.
class UpdatePacing final : public RuntimeOnlyPatch {
  public:
    explicit UpdatePacing(const TargetContext& target) noexcept;
    ~UpdatePacing() override;

    [[nodiscard]] std::expected<void, OutcomeReason> prepare_runtime() override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

    // Brackets one native fragment group emitted for a Direct association.
    void begin_group(std::uint8_t slot, std::uint32_t generation, int packet_type) noexcept;
    // Copies only early complete-update fragments; all other packets remain synchronous.
    [[nodiscard]] direct_transport::server::PacingPacketAction
    route_packet(std::uint8_t slot, std::uint32_t generation, std::span<const std::uint8_t> bytes,
                 const direct_transport::server::PacedPacketSender& sender) noexcept;
    void end_group(std::uint8_t slot, std::uint32_t generation,
                   const direct_transport::server::PacedPacketSender& sender) noexcept;
    // Releases deferred groups from Direct Transport's next network-owner callback.
    void service(const direct_transport::server::PacedPacketSender& sender) noexcept;
    // Drops deferred bytes when their association generation ends.
    void discard(std::uint8_t slot, std::uint32_t generation) noexcept;

  private:
    using Clock = std::chrono::steady_clock;

    static constexpr std::size_t kMaximumPlayers = direct_transport::kPhysicalAssociationCount;
    static constexpr std::size_t kMaximumGroupFragments = direct_transport::kMaximumNativeGroupFragments;
    static constexpr std::uint8_t kCompleteUpdatePacketType = 0x0C;

    enum class GroupMode {
        None,
        Immediate,
        Capturing,
    };

    struct Fragment {
        std::uint16_t size;
        std::array<std::uint8_t, direct_transport::kMaximumNativeBytes> bytes;
    };

    // Keeps one release anchor and at most one complete deferred group per player.
    struct SlotState {
        std::array<Fragment, kMaximumGroupFragments> fragments;
        Clock::time_point previous_release{};
        Clock::time_point completion{};
        std::uint32_t generation{};
        std::uint32_t bytes{};
        std::uint32_t current_bytes{};
        std::uint16_t fragment_count{};
        std::uint16_t current_fragment_count{};
        network_pipeline::OutputPacingOutcome pending_outcome{};
        GroupMode group_mode{GroupMode::None};
        bool has_previous_release{};
        bool pending{};
        bool current_eligible{};
        bool complete_update_group{};
    };

    using SlotStates = std::array<SlotState, kMaximumPlayers>;

    [[nodiscard]] PacingMode current_mode() const noexcept;
    void refresh_mode(PacingMode mode, const direct_transport::server::PacedPacketSender& sender) noexcept;
    [[nodiscard]] SlotState& state_for(std::uint8_t slot, std::uint32_t generation) noexcept;
    [[nodiscard]] bool append(SlotState& state, std::span<const std::uint8_t> bytes) noexcept;
    void release(std::uint8_t slot, SlotState& state, network_pipeline::OutputPacingOutcome outcome,
                 const direct_transport::server::PacedPacketSender& sender) noexcept;
    void observe(std::uint8_t slot, std::uint32_t generation, network_pipeline::OutputPacingOutcome outcome,
                 std::uint16_t fragment_count, std::uint32_t bytes, Clock::time_point completion,
                 Clock::time_point release) const noexcept;
    void add_pending(SlotState& state) noexcept;
    static void reset(SlotState& state, std::uint32_t generation = 0) noexcept;

    const float* fixed_delta_{};
    direct_transport::server::OutputPacingCallbacks callbacks_;
    std::unique_ptr<SlotStates> states_;
    PacingMode mode_{PacingMode::Unsupported};
    std::size_t pending_count_{};
};

} // namespace fusioncutter::patches::update_pacing
