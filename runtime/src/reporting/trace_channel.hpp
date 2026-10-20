#pragma once

#include "tracing.hpp"
#include "byte_ring.hpp"

#include <Windows.h>

#include <atomic>
#include <string>

namespace fc::runtime {

namespace trace_detail {

// Leaves room for event headers within ETW's per-event size limit.
inline constexpr std::size_t kMaximumPayload = 60U * 1024U;

// Stores the producer metadata copied into each queued trace record.
struct Envelope {
    // Timestamp is in QueryPerformanceCounter ticks.
    std::uint64_t timestamp{};
    std::uint32_t thread{}, payload_size{};
};

} // namespace trace_detail

// Retains a channel's queue, identity and counters for producers and the session's output worker.
struct TraceChannel::State {
    const PatchRecord *owner{};
    std::string name;
    std::uint32_t version{};
    reporting::ByteRing queue;
    // Sequentially consistent admission checks protect the final shutdown drain and storage reclamation after discard.
    std::atomic<std::uint32_t> active_writes{};
    // Session event requests another worker pass after copying or discard.
    HANDLE wake{};
    // Assigned by the sole consumer when extracting accepted records, in reservation order.
    std::uint64_t sequence{};
    std::atomic_bool disabled{};
    // Set by the output worker after the channel metadata has been submitted to ETW.
    bool definition_written{};
    // Shared session flags remain valid for the lifetime of this channel.
    const std::atomic_bool *output_failed{}, *file_limit_reached{}, *admission_closed{};
    std::atomic<std::uint64_t> accepted{}, written{}, dropped{};
    // Queue exhaustion is separate from output losses; only the worker changes the warning flag.
    std::atomic<std::uint64_t> queue_full{};
    bool queue_warning_written{};
};

} // namespace fc::runtime
