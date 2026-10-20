#pragma once

#include "status.hpp"

#include <fc/abi.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace fc::runtime {

struct PatchRecord;

}

namespace fc::reporting {

// Queues log records and status reports for a worker that formats and writes them to disk.
// Log submission uses 512 preallocated slots and never waits for the worker.
class ReportingSession final {
  public:
    // Creates an unstarted session with the Warning severity filter.
    ReportingSession();
    ReportingSession(const ReportingSession &) = delete;
    ReportingSession &operator=(const ReportingSession &) = delete;
    // Finishes output before releasing storage; stop producer calls before destroying the session.
    ~ReportingSession();

    // Closes admission, drains accepted logs and diagnostics, joins the worker and closes file output once.
    // Call outside the worker; concurrent calls wait, later submissions are refused, and status remains available.
    void shutdown() noexcept;

    // Starts the worker once, before producer calls; supply a nonempty output directory.
    // Initialization failures are reported through debugger/console diagnostics and status().
    void start(SessionFacts facts, std::filesystem::path output_directory) noexcept;
    // Sets the severity filter for subsequent admission checks; Off suppresses log records.
    void configure(FC_LogLevel level) noexcept;

    // Reports whether admission is open and the configured severity filter admits this level.
    bool enabled(FC_LogLevel level) const noexcept;

    // Returns true after copying packet bytes; the 4 KiB limit includes the record header.
    // Keep owner and formatter valid until exit; filtering, missing formatters and nonempty null payloads refuse without loss.
    bool submit(const runtime::PatchRecord *owner, FC_LogLevel level, FC_LogPacket packet) noexcept;
    // Copies text with the same queue and owner lifetime requirements as submit(), using no deferred formatter.
    bool write(const runtime::PatchRecord *owner, FC_LogLevel level, FC_Text text) noexcept;

    // Supplies a complete report from the control thread; a newer pending report replaces the previous one.
    void publish_status(std::string content) noexcept;
    // Samples configuration, record counts and output health; counters may change independently during the call.
    LogStatus status() const noexcept;

    // Reports changed diagnostic text through a separate slot, honoring the file severity filter.
    // Until shutdown, error fallback remains available independently of worker readiness and the file filter.
    void diagnostic(std::string_view reason, std::uint32_t windows_error = 0, FC_LogLevel level = FC_LOG_ERROR) noexcept;

  private:
    class State;

    std::unique_ptr<State> state_;
};

} // namespace fc::reporting
