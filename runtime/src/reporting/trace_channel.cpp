#include "trace_channel.hpp"

namespace fc::runtime {

using namespace trace_detail;

TraceChannel::TraceChannel(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

TraceChannel::~TraceChannel() = default;

bool TraceChannel::enabled() const noexcept {
    return !state_->disabled && !state_->admission_closed->load() && !state_->output_failed->load() && !state_->file_limit_reached->load();
}

FC_TraceHealth TraceChannel::health() const noexcept {
    return {state_->accepted.load(), state_->written.load(), state_->dropped.load(), state_->output_failed->load() ? 1U : 0U,
            state_->file_limit_reached->load() ? 1U : 0U};
}

bool TraceChannel::write(FC_Bytes record) noexcept {
    if ((!record.data && record.size) || record.size > kMaximumPayload) {
        return false;
    }
    if (state_->disabled || state_->admission_closed->load()) {
        return false;
    }

    // Recheck admission after entering so shutdown and discard can wait for every producer that may publish.
    state_->active_writes.fetch_add(1);
    const bool queued = [&]() noexcept {
        if (state_->disabled.load() || state_->admission_closed->load()) {
            return false;
        }
        // An output failure or file limit turns subsequent submissions into reported output losses.
        if (state_->output_failed->load() || state_->file_limit_reached->load()) {
            state_->dropped.fetch_add(1);
            return false;
        }

        // Capture the producer's time and thread before copying; the worker supplies the ordered sequence later.
        LARGE_INTEGER timestamp{};
        QueryPerformanceCounter(&timestamp);
        const Envelope envelope{static_cast<std::uint64_t>(timestamp.QuadPart), GetCurrentThreadId(), record.size};
        const auto reservation = state_->queue.reserve(static_cast<std::uint32_t>(sizeof(Envelope)) + record.size);
        if (!reservation) {
            state_->queue_full.fetch_add(1);
            state_->dropped.fetch_add(1);
            return false;
        }

        state_->queue.copy_in(*reservation, 0, &envelope, sizeof(envelope));
        state_->queue.copy_in(*reservation, sizeof(envelope), record.data, record.size);
        state_->accepted.fetch_add(1);
        state_->queue.publish(*reservation);
        return true;
    }();

    // A publishing or last departing producer wakes the worker; unfinished reservations never make it spin.
    state_->active_writes.fetch_sub(1);
    SetEvent(state_->wake);
    return queued;
}

} // namespace fc::runtime
