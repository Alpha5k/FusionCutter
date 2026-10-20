#pragma once

#include <fc/abi.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fc::reporting {

// Identifies the process and startup time shared by this session's reports.
struct SessionFacts {
    std::chrono::system_clock::time_point started = std::chrono::system_clock::now();
    FC_Role role = FC_ROLE_CLIENT;
    std::string executable_basename;
};

// Captures the logger's configuration, counters and current output state.
struct LogStatus {
    FC_LogLevel level = FC_LOG_WARNING;
    std::filesystem::path path;
    // Record counts: queued, successfully written (including summarized repeats), and lost.
    std::uint64_t accepted{}, written{}, dropped{};
    bool output_failed{};
};

// Summarizes the shared trace session for the runtime status report.
struct TraceStatus {
    bool requested{}, configured_disabled{};
    std::optional<std::filesystem::path> path;
    // Channel-level drops followed by event and buffer losses reported by ETW.
    std::uint64_t dropped{}, events_lost{}, buffers_lost{};
    bool file_limit_reached{}, output_failed{};
};

// Collects status text on the control thread and marks truncation at the report's byte limit.
class StatusText final {
  public:
    // Appends normalized text and a newline; returns false once the report is truncated.
    bool line(std::string_view value);
    // Appends text with ASCII controls replaced by spaces; truncation preserves complete valid UTF-8 characters.
    // Returns false after adding the truncation marker and refuses later appends.
    bool append(std::string_view value);

    std::string_view content() const noexcept {
        return text_;
    }

    // Returns the collected report as an owned string.
    std::string take() && noexcept {
        return std::move(text_);
    }

    // The writer borrows this collector for a synchronous owner callback, preserving submitted line breaks.
    FC_StatusWriter writer() noexcept;

  private:
    // Adapts a synchronous status line callback; context must point to a live collector.
    static std::uint32_t FC_CDECL write_line(void *, FC_Text);

    std::string text_;
    bool truncated_{};
};

enum class StatusPublishOutcome {
    Unchanged,
    Written,
    Failed
};

struct StatusPublishResult {
    StatusPublishOutcome outcome{};
    std::string_view failure;
    std::uint32_t windows_error{};
};

// Writes changed status reports and remembers the last successful content; used by the reporting worker.
class StatusPublisher final {
  public:
    explicit StatusPublisher(std::filesystem::path path);
    // Adds the publication time when content changes; failed writes remain eligible for retry.
    StatusPublishResult publish(std::string_view semantic_content) noexcept;

  private:
    std::filesystem::path path_;
    std::optional<std::string> successful_;
};

} // namespace fc::reporting
