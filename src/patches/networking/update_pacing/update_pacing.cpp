#include "update_pacing.hpp"

#include <cstring>
#include <new>

namespace fusioncutter::patches::update_pacing {
namespace {

constexpr std::uint32_t kGogFixedDeltaRva = 0x01A6'4368;

[[nodiscard]] std::uint64_t timestamp_ns(std::chrono::steady_clock::time_point time) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

[[nodiscard]] direct_transport::server::OutputPacingCallbacks make_callbacks(UpdatePacing& pacing) noexcept {
    using direct_transport::server::PacedPacketSender;
    return {
        .context = &pacing,
        .begin_group =
            [](void* context, std::uint8_t slot, std::uint32_t generation, int packet_type) noexcept {
                static_cast<UpdatePacing*>(context)->begin_group(slot, generation, packet_type);
            },
        .route_packet =
            [](void* context, std::uint8_t slot, std::uint32_t generation, std::span<const std::uint8_t> bytes,
               const PacedPacketSender& sender) noexcept {
                return static_cast<UpdatePacing*>(context)->route_packet(slot, generation, bytes, sender);
            },
        .end_group =
            [](void* context, std::uint8_t slot, std::uint32_t generation, const PacedPacketSender& sender) noexcept {
                static_cast<UpdatePacing*>(context)->end_group(slot, generation, sender);
            },
        .service =
            [](void* context, const PacedPacketSender& sender) noexcept {
                static_cast<UpdatePacing*>(context)->service(sender);
            },
        .discard =
            [](void* context, std::uint8_t slot, std::uint32_t generation) noexcept {
                static_cast<UpdatePacing*>(context)->discard(slot, generation);
            },
    };
}

} // namespace

UpdatePacing::UpdatePacing(const TargetContext& target) noexcept
    : fixed_delta_(target.image.read_at_rva<float>(kGogFixedDeltaRva)), callbacks_(make_callbacks(*this)) {}

UpdatePacing::~UpdatePacing() {
    disable_runtime();
}

std::expected<void, OutcomeReason> UpdatePacing::prepare_runtime() {
    if (fixed_delta_ == nullptr) {
        return std::unexpected(OutcomeReason{
            "The server update interval is outside the recognized image", "Read server update interval", {}});
    }
    try {
        states_ = std::make_unique_for_overwrite<SlotStates>();
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            OutcomeReason{"Update Pacing could not allocate its bounded group storage", "Prepare Update Pacing", {}});
    }
    return {};
}

void UpdatePacing::enable_runtime() noexcept {
    mode_ = current_mode();
    direct_transport::server::publish_output_pacing(callbacks_);
}

void UpdatePacing::disable_runtime() noexcept {
    direct_transport::server::clear_output_pacing(callbacks_);
    if (states_ != nullptr) {
        for (auto& state : *states_) {
            reset(state);
        }
    }
    mode_ = PacingMode::Unsupported;
    pending_count_ = 0;
}

void UpdatePacing::begin_group(std::uint8_t slot, std::uint32_t generation, int packet_type) noexcept {
    if (slot >= states_->size()) {
        return;
    }
    auto& state = state_for(slot, generation);
    state.group_mode = GroupMode::None;
    state.current_fragment_count = 0;
    state.current_bytes = 0;
    state.current_eligible = false;
    state.complete_update_group = packet_type == kCompleteUpdatePacketType;
}

direct_transport::server::PacingPacketAction
UpdatePacing::route_packet(std::uint8_t slot, std::uint32_t generation, std::span<const std::uint8_t> bytes,
                           const direct_transport::server::PacedPacketSender& sender) noexcept {
    using direct_transport::server::PacingPacketAction;
    if (slot >= states_->size() || bytes.empty()) {
        return PacingPacketAction::SendNow;
    }

    auto& existing = state_for(slot, generation);
    if (existing.group_mode == GroupMode::Immediate) {
        if (existing.current_eligible) {
            ++existing.current_fragment_count;
            existing.current_bytes += static_cast<std::uint32_t>(bytes.size());
        }
        return PacingPacketAction::SendNow;
    }
    if (existing.group_mode == GroupMode::Capturing) {
        if (append(existing, bytes)) {
            return PacingPacketAction::Buffered;
        }
        existing.completion = Clock::now();
        release(slot, existing, network_pipeline::OutputPacingOutcome::CapacityExceeded, sender);
        existing.group_mode = GroupMode::Immediate;
        existing.current_fragment_count = 1;
        existing.current_bytes = static_cast<std::uint32_t>(bytes.size());
        existing.current_eligible = true;
        return PacingPacketAction::SendNow;
    }

    if (!existing.complete_update_group || bytes.size() < direct_transport::kNativeHeaderBytes ||
        bytes.size() > direct_transport::kMaximumNativeBytes) {
        existing.group_mode = GroupMode::Immediate;
        return PacingPacketAction::SendNow;
    }

    const auto mode = current_mode();
    refresh_mode(mode, sender);
    auto& state = state_for(slot, generation);

    if (mode == PacingMode::Unsupported) {
        state.group_mode = GroupMode::Immediate;
        return PacingPacketAction::SendNow;
    }

    if (state.pending) {
        release(slot, state, network_pipeline::OutputPacingOutcome::QueueCollision, sender);
        state.group_mode = GroupMode::Immediate;
        state.current_fragment_count = 1;
        state.current_bytes = static_cast<std::uint32_t>(bytes.size());
        state.current_eligible = true;
        return PacingPacketAction::SendNow;
    }

    const auto now = Clock::now();
    if (!state.has_previous_release || !pacing_decision(state.previous_release, now, mode).hold) {
        state.group_mode = GroupMode::Immediate;
        state.current_fragment_count = 1;
        state.current_bytes = static_cast<std::uint32_t>(bytes.size());
        state.current_eligible = true;
        return PacingPacketAction::SendNow;
    }

    state.group_mode = GroupMode::Capturing;
    state.completion = {};
    state.fragment_count = 0;
    state.bytes = 0;
    static_cast<void>(append(state, bytes));
    return PacingPacketAction::Buffered;
}

void UpdatePacing::end_group(std::uint8_t slot, std::uint32_t generation,
                             const direct_transport::server::PacedPacketSender& sender) noexcept {
    if (slot >= states_->size()) {
        return;
    }
    auto& state = (*states_)[slot];
    if (state.generation != generation) {
        return;
    }

    if (state.group_mode == GroupMode::Immediate) {
        if (state.current_eligible) {
            const auto completion = Clock::now();
            state.previous_release = completion;
            state.has_previous_release = true;
            observe(slot, state.generation, network_pipeline::OutputPacingOutcome::Immediate,
                    state.current_fragment_count, state.current_bytes, completion, completion);
        }
        state.group_mode = GroupMode::None;
        state.current_eligible = false;
        return;
    }
    if (state.group_mode != GroupMode::Capturing || state.fragment_count == 0) {
        state.group_mode = GroupMode::None;
        return;
    }

    const auto completion = Clock::now();
    state.completion = completion;
    const auto decision = pacing_decision(state.previous_release, completion, mode_);
    if (!decision.hold) {
        release(slot, state, network_pipeline::OutputPacingOutcome::Immediate, sender);
        return;
    }

    state.pending_outcome = decision.cap_limited ? network_pipeline::OutputPacingOutcome::CapLimited
                                                 : network_pipeline::OutputPacingOutcome::Held;
    add_pending(state);
    state.group_mode = GroupMode::None;
}

void UpdatePacing::service(const direct_transport::server::PacedPacketSender& sender) noexcept {
    if (pending_count_ == 0) {
        return;
    }
    const auto mode = current_mode();
    if (mode != mode_) {
        refresh_mode(mode, sender);
        return;
    }
    for (std::uint8_t slot = 0; slot < states_->size(); ++slot) {
        auto& state = (*states_)[slot];
        if (state.pending) {
            release(slot, state, state.pending_outcome, sender);
        }
    }
}

void UpdatePacing::discard(std::uint8_t slot, std::uint32_t generation) noexcept {
    if (slot >= states_->size()) {
        return;
    }
    auto& state = (*states_)[slot];
    if (state.generation != generation) {
        return;
    }
    if (state.pending) {
        observe(slot, state.generation, network_pipeline::OutputPacingOutcome::LifecycleDiscard, state.fragment_count,
                state.bytes, state.completion, {});
        --pending_count_;
    }
    reset(state);
}

PacingMode UpdatePacing::current_mode() const noexcept {
    return fixed_delta_ == nullptr ? PacingMode::Unsupported : pacing_mode(*fixed_delta_);
}

void UpdatePacing::refresh_mode(PacingMode mode, const direct_transport::server::PacedPacketSender& sender) noexcept {
    if (mode == mode_) {
        return;
    }
    for (std::uint8_t slot = 0; slot < states_->size(); ++slot) {
        auto& state = (*states_)[slot];
        if (state.pending) {
            release(slot, state, network_pipeline::OutputPacingOutcome::ModeTransition, sender);
        }
        reset(state, state.generation);
    }
    pending_count_ = 0;
    mode_ = mode;
}

UpdatePacing::SlotState& UpdatePacing::state_for(std::uint8_t slot, std::uint32_t generation) noexcept {
    auto& state = (*states_)[slot];
    if (state.generation != generation) {
        if (state.pending) {
            observe(slot, state.generation, network_pipeline::OutputPacingOutcome::LifecycleDiscard,
                    state.fragment_count, state.bytes, state.completion, {});
            --pending_count_;
        }
        reset(state, generation);
    }
    return state;
}

bool UpdatePacing::append(SlotState& state, std::span<const std::uint8_t> bytes) noexcept {
    if (state.fragment_count >= state.fragments.size() || bytes.size() > direct_transport::kMaximumNativeBytes) {
        return false;
    }
    auto& fragment = state.fragments[state.fragment_count++];
    fragment.size = static_cast<std::uint16_t>(bytes.size());
    std::memcpy(fragment.bytes.data(), bytes.data(), bytes.size());
    state.bytes += static_cast<std::uint32_t>(bytes.size());
    return true;
}

void UpdatePacing::release(std::uint8_t slot, SlotState& state, network_pipeline::OutputPacingOutcome outcome,
                           const direct_transport::server::PacedPacketSender& sender) noexcept {
    for (std::size_t index = 0; index < state.fragment_count; ++index) {
        const auto& fragment = state.fragments[index];
        sender.send(sender.context, slot, std::span{fragment.bytes}.first(fragment.size));
    }
    const auto released = Clock::now();
    state.previous_release = released;
    state.has_previous_release = true;
    observe(slot, state.generation, outcome, state.fragment_count, state.bytes, state.completion, released);
    if (state.pending) {
        --pending_count_;
    }
    state.pending = false;
    state.fragment_count = 0;
    state.bytes = 0;
    state.group_mode = GroupMode::None;
}

void UpdatePacing::add_pending(SlotState& state) noexcept {
    state.pending = true;
    ++pending_count_;
}

void UpdatePacing::observe(std::uint8_t slot, std::uint32_t generation, network_pipeline::OutputPacingOutcome outcome,
                           std::uint16_t fragment_count, std::uint32_t bytes, Clock::time_point completion,
                           Clock::time_point release) const noexcept {
    network_pipeline::observe_output_pacing({
        .slot = slot,
        .generation = generation,
        .outcome = outcome,
        .fragment_count = fragment_count,
        .bytes = bytes,
        .completion_ns = timestamp_ns(completion),
        .release_ns = timestamp_ns(release),
    });
}

void UpdatePacing::reset(SlotState& state, std::uint32_t generation) noexcept {
    state.previous_release = {};
    state.completion = {};
    state.generation = generation;
    state.bytes = 0;
    state.current_bytes = 0;
    state.fragment_count = 0;
    state.current_fragment_count = 0;
    state.group_mode = GroupMode::None;
    state.has_previous_release = false;
    state.pending = false;
    state.current_eligible = false;
    state.complete_update_group = false;
}

} // namespace fusioncutter::patches::update_pacing
