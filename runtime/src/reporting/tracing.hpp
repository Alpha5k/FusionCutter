#pragma once

#include "status.hpp"
#include "diagnostic.hpp"

#include <fc/abi.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>

namespace fc::reporting {

class ReportingSession;

}

namespace fc::runtime {

struct PatchRecord;
class TraceSession;

// Queues one owner's binary records for the session's trace worker and tracks their acceptance and loss.
class TraceChannel final {
  public:
    ~TraceChannel();

    // Reports whether the channel and session allow submissions; a later write can still be refused.
    bool enabled() const noexcept;

    // Copies at most 60 KiB from readable record storage before returning, without allocation; true means queued for output.
    // Refused records count as dropped unless malformed, oversized, disabled or submitted after shutdown.
    bool write(FC_Bytes record) noexcept;

    // Returns counters and output flags. written counts successful ETW submissions; dropped counts queue and output losses.
    FC_TraceHealth health() const noexcept;

  private:
    struct State;
    // Takes ownership of the state retained by this session-owned channel.
    explicit TraceChannel(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
    friend class TraceSession;
};

// Owns channels and a worker that records their metadata and queued bytes with Event Tracing for Windows (ETW).
class TraceSession final {
  public:
    // Creates a session with a compressed file limit rounded up from bytes to MiB; the final ETW buffer can extend beyond it.
    // Zero disables recording while still allowing channel registration.
    explicit TraceSession(std::uint64_t max_trace_bytes = 512ULL * 1024U * 1024U);
    TraceSession(const TraceSession &) = delete;
    TraceSession &operator=(const TraceSession &) = delete;

    // Stop producer calls first. Destruction finishes output and releases channel storage.
    ~TraceSession();

    // Closes admission, drains admitted records, joins the worker and finalizes ETW once; concurrent calls wait for completion.
    // Channel pointers and status queries remain valid until destruction, and later submissions are refused without loss.
    void shutdown() noexcept;

    // Configure once before channel creation; output goes beneath directory/traces on the first drained record.
    // The optional diagnostic session must outlive this session.
    void configure(std::uint64_t max_trace_bytes, std::filesystem::path directory,
                   reporting::ReportingSession *diagnostics = nullptr) noexcept;

    // Returns a session-owned channel or Diagnostic; name must be nonempty UTF-8 without ASCII controls and queue budget in bytes must be
    // nonzero. Names are copied and unique per owner. The owner and its declarations must outlive the session; null means runtime traces.
    std::expected<TraceChannel *, Diagnostic> create_channel(const PatchRecord *owner, FC_TraceDefinition definition) noexcept;

    // Call after a failed owner's handler is destroyed. Disables its channels; the worker counts queued records as dropped.
    // Channel pointers remain valid until session destruction; a record already copied by the worker may finish.
    void discard(const PatchRecord *owner) noexcept;

    // Returns current output flags, aggregate channel drops and the latest queried ETW losses; path appears after setup.
    reporting::TraceStatus status() const noexcept;

  private:
    struct State;

    std::unique_ptr<State> state_;
};

} // namespace fc::runtime
