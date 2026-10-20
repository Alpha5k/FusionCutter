#include "reporting.hpp"

#include "log_writer.hpp"

#include "bounded_text.hpp"
#include "patches/patch_record.hpp"

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace fc::reporting {

namespace {

constexpr std::size_t kQueueBytes = 2U * 1024U * 1024U;
constexpr std::size_t kRecordBytes = 4U * 1024U;

// Prefixes each copied payload with the facts needed to format and attribute it on the worker.
struct Record {
    const runtime::PatchRecord *owner{};
    FC_LogFormatFn formatter{};
    // Nanoseconds since the system clock epoch, captured by the producer.
    std::int64_t timestamp{};
    std::uint32_t payload_size{};
    FC_LogLevel level{};
};

static_assert(sizeof(Record) < kRecordBytes);

// Each slot stores a record of up to 4 KiB, plus its Windows list entry.
struct alignas(MEMORY_ALLOCATION_ALIGNMENT) RecordSlot {
    SLIST_ENTRY entry{};
    Record record{};
    std::array<std::byte, kRecordBytes - sizeof(Record)> payload{};
};

// Fixed slots reserve 512 records from the 2 MiB budget, including when their payloads are short.
constexpr std::size_t kQueueSlots = kQueueBytes / kRecordBytes;
static_assert(offsetof(RecordSlot, entry) == 0);
static_assert(sizeof(RecordSlot) == kRecordBytes + MEMORY_ALLOCATION_ALIGNMENT);

using fc::detail::view;

// Supplies the log label for a severity, using Off for values outside the supported levels.
const char *severity(FC_LogLevel level) noexcept {
    switch (level) {
    case FC_LOG_ERROR:
        return "Error";
    case FC_LOG_WARNING:
        return "Warning";
    case FC_LOG_INFO:
        return "Info";
    case FC_LOG_DEBUG:
        return "Debug";
    default:
        return "Off";
    }
}

// Returns the handler's diagnostic name or Plugin/Patch; logs without an owner use Fusion Cutter.
std::string source(const runtime::PatchRecord *owner) {
    if (!owner) {
        return "Fusion Cutter";
    }
    if (owner->internal_factory.create) {
        return owner->resolved.owner;
    }

    return std::format("{}/{}", view(owner->plugin->admitted.definition->id), view(owner->definition->id));
}

// Reports a failure through the debugger and an attached console when file output is unavailable.
void fallback(std::string_view message, std::uint32_t code, FC_LogLevel level) noexcept {
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.remove_suffix(1);
    }
    std::array<char, 1024> buffer{};
    const auto prefix =
        static_cast<std::size_t>(_snprintf_s(buffer.data(), buffer.size(), _TRUNCATE, "[%s] [Fusion Cutter] ", severity(level)));
    constexpr std::string_view marker = " [truncated]";
    const bool truncated = message.size() > 900;
    const auto copied = prefix + copy_structured_bytes(std::span{buffer}.subspan(prefix, truncated ? 900 - marker.size() : 900), message);
    auto position = copied;
    if (truncated) {
        std::memcpy(buffer.data() + position, marker.data(), marker.size());
        position += marker.size();
    }
    const auto count = code ? _snprintf_s(buffer.data() + position, buffer.size() - position, _TRUNCATE, " (Windows error %lu)\r\n",
                                          static_cast<unsigned long>(code))
                            : _snprintf_s(buffer.data() + position, buffer.size() - position, _TRUNCATE, "\r\n");
    const auto length = position + (count > 0 ? static_cast<std::size_t>(count) : 0);
    // ABI text is UTF-8; debugger and console calls consume UTF-16 without depending on the active code page.
    std::array<wchar_t, 1024> wide{};
    const auto characters =
        MultiByteToWideChar(CP_UTF8, 0, buffer.data(), static_cast<int>(length), wide.data(), static_cast<int>(wide.size() - 1));
    OutputDebugStringW(wide.data());

    const auto console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode{}, written{};
    if (console && console != INVALID_HANDLE_VALUE && GetConsoleMode(console, &mode)) {
        WriteConsoleW(console, wide.data(), static_cast<DWORD>(characters), &written, nullptr);
    }
}

// Appends a formatter's borrowed text to its worker-owned string; returns 0 on invalid input or failure.
std::uint32_t FC_CDECL append_formatted(void *context, FC_Text text) {
    if (!context || (!text.data && text.size)) {
        return 0;
    }

    auto &output = *static_cast<std::string *>(context);

    // Limit accumulated formatter output, including padding added by the format string.
    if (text.size > kFileBytes - std::min<std::uint64_t>(output.size(), kFileBytes)) {
        return 0;
    }

    try {
        output.append(view(text));
        return 1;
    } catch (...) {
        return 0;
    }
}

// Uses the same timestamp, severity and owner prefix for queued records and direct worker diagnostics.
std::string format_line(Record record, std::string_view message) {
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.remove_suffix(1);
    }
    std::string normalized{message.empty() ? "No log message was supplied" : message};
    // Keep borrowed provider text and controls on this record's attributed physical line.
    copy_structured_bytes(std::span{normalized.data(), normalized.size()}, normalized);
    const auto time = std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{record.timestamp})};
    return std::format("{:%Y-%m-%d %H:%M:%S} [{}] [{}] {}\r\n", time, severity(record.level), source(record.owner), normalized);
}

} // namespace

// Coordinates producers with one worker that owns file output and deferred formatting.
class ReportingSession::State final {
  public:
    // Wakes the worker to drain queued logs and joins it before releasing the wake event.
    ~State() {
        shutdown();
        if (wake_) {
            CloseHandle(wake_);
        }
    }

    // Retains public state while finalizing accepted work and the worker-owned file handle.
    void shutdown() noexcept {
        std::scoped_lock shutdown_lock(shutdown_mutex_);
        {
            std::scoped_lock status_lock(status_mutex_);
            admission_closed_.store(true);
        }
        if (wake_) {
            SetEvent(wake_);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        writer_.reset();
    }

    // Starts this state once before producer calls; initialization failures go through the diagnostic fallback.
    void start(SessionFacts facts, std::filesystem::path directory) noexcept {
        std::scoped_lock lock(shutdown_mutex_);
        if (admission_closed_.load()) {
            return;
        }
        try {
            path_ = directory / "FusionCutter.log";
            slots_ = std::make_unique<RecordSlot[]>(kQueueSlots);
            InitializeSListHead(&free_);
            InitializeSListHead(&pending_);
            for (std::size_t index = 0; index != kQueueSlots; ++index) {
                InterlockedPushEntrySList(&free_, &slots_[index].entry);
            }
            publisher_ = std::make_unique<StatusPublisher>(directory / "FusionCutter.txt");
            writer_ = std::make_unique<LogWriter>(directory, facts);
            wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!wake_) {
                failed_.store(true);
                diagnostic("The reporting worker event could not be created", GetLastError());
                return;
            }

            // Publish readiness only after storage, output objects and the worker have been created.
            worker_ = std::thread{[this] {
                run();
            }};
            ready_.store(true, std::memory_order_release);
        } catch (...) {
            failed_.store(true);
            diagnostic("The reporting worker could not be initialized", ERROR_NOT_ENOUGH_MEMORY);
        }
    }

    // Tests the severity filter independently of worker readiness and output failures.
    bool enabled(FC_LogLevel level) const noexcept {
        return level >= FC_LOG_ERROR && level <= FC_LOG_DEBUG && level <= level_.load(std::memory_order_relaxed);
    }

    // Copies an admitted record into a free slot without waiting for another thread; owners outlive worker use.
    bool enqueue(const runtime::PatchRecord *owner, FC_LogLevel level, FC_Bytes bytes, FC_LogFormatFn formatter) noexcept {
        if (admission_closed_.load()) {
            return false;
        }
        // A producer that saw open admission must recheck after entering before it can publish a record.
        active_writes_.fetch_add(1);
        const bool queued = !admission_closed_.load() && copy_record(owner, level, bytes, formatter);
        active_writes_.fetch_sub(1);
        if (wake_ && (queued || admission_closed_.load())) {
            SetEvent(wake_);
        }
        return queued;
    }

    // Copies one record after admission; the producer remains active through publication.
    bool copy_record(const runtime::PatchRecord *owner, FC_LogLevel level, FC_Bytes bytes, FC_LogFormatFn formatter) noexcept {
        // Filter before counting refusals so intentionally suppressed messages do not increase loss totals.
        if (!enabled(level)) {
            return false;
        }
        if ((!bytes.data && bytes.size)) {
            return false;
        }

        if (bytes.size > kRecordBytes - sizeof(Record) || !ready_.load(std::memory_order_acquire) || failed_.load()) {
            lose();
            return false;
        }

        // Each producer owns its slot until it publishes the completed record.
        const auto entry = InterlockedPopEntrySList(&free_);
        if (!entry) {
            lose();
            return false;
        }

        auto &slot = *reinterpret_cast<RecordSlot *>(entry);
        slot.record =
            Record{owner, formatter,
                   std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
                   bytes.size, level};

        if (bytes.size) {
            std::memcpy(slot.payload.data(), bytes.data, bytes.size);
        }
        accepted_.fetch_add(1);
        InterlockedPushEntrySList(&pending_, entry);
        return true;
    }

    // Replaces pending status content under its own lock and wakes the worker to publish it.
    void publish(std::string content) noexcept {
        {
            std::scoped_lock lock(status_mutex_);
            if (admission_closed_.load()) {
                return;
            }
            pending_status_ = std::move(content);
        }

        if (wake_) {
            SetEvent(wake_);
        }
    }

    // Retains the latest changed reporting notice independently of queued logs.
    // A new file outage reports again even when its reason matches the previous outage.
    void diagnostic(std::string_view reason, DWORD code, FC_LogLevel level = FC_LOG_ERROR, bool new_outage = false) noexcept {
        while (!reason.empty() && (reason.back() == '\r' || reason.back() == '\n')) {
            reason.remove_suffix(1);
        }
        if (reason.empty()) {
            reason = "No diagnostic message was supplied";
        }
        std::array<char, 768> detail{};
        constexpr std::string_view marker = " [truncated]";
        const bool truncated = reason.size() >= detail.size();
        auto size = copy_structured_bytes(std::span{detail}.first(detail.size() - 1 - (truncated ? marker.size() : 0)), reason);
        if (truncated) {
            std::memcpy(detail.data() + size, marker.data(), marker.size());
        }
        // The slot is independent of the normal queue and may overwrite an older explanation under pressure.
        bool changed = true;
        std::unique_lock lock(diagnostic_mutex_, std::try_to_lock);
        if (lock) {
            changed = new_outage || code != diagnostic_code_ || level != diagnostic_level_ || detail != diagnostic_text_;
            if (changed) {
                diagnostic_text_ = detail;
                diagnostic_code_ = code;
                diagnostic_level_ = level;
                diagnostic_pending_ = true;
            }
        }
        if (lock) {
            lock.unlock();
        }

        // Startup can fail before a worker exists, so the useful reason cannot depend on later slot consumption.
        if (changed && (level == FC_LOG_ERROR || enabled(level))) {
            fallback(reason, code, level);
        }

        if (changed && wake_) {
            SetEvent(wake_);
        }
    }

    // Samples counters and output state; inability to copy the path produces a failed-output snapshot.
    LogStatus status() const noexcept {
        try {
            return {level_.load(), path_, accepted_.load(), written_.load(), dropped_.load(), failed_.load()};
        } catch (...) {
            return {.output_failed = true};
        }
    }

    std::atomic<FC_LogLevel> level_{FC_LOG_WARNING};
    std::atomic_bool admission_closed_{};

    // Keeps an admitted external diagnostic active until its slot and fallback have been updated.
    void submit_diagnostic(std::string_view reason, DWORD code, FC_LogLevel level) noexcept {
        if (admission_closed_.load()) {
            return;
        }
        active_writes_.fetch_add(1);
        if (!admission_closed_.load()) {
            diagnostic(reason, code, level);
        }
        active_writes_.fetch_sub(1);
        if (wake_ && admission_closed_.load()) {
            SetEvent(wake_);
        }
    }

  private:
    // Counts records lost at admission or output and wakes the worker to report the new total.
    void lose(std::uint64_t count = 1) noexcept {
        dropped_.fetch_add(count);
        if (wake_) {
            SetEvent(wake_);
        }
    }

    // Writes a prefixed record and counts represented records as written or lost; failures report a diagnostic.
    bool write_line(Record record, std::string_view message, std::uint64_t count) noexcept {
        try {
            const auto line = format_line(record, message);
            DWORD code{};
            if (writer_->write(line, code)) {
                written_.fetch_add(count);
                failed_.store(false);
                return true;
            } else {
                lose(count);
                const bool new_outage = !failed_.exchange(true);
                diagnostic("Could not write a queued record to FusionCutter.log", code, FC_LOG_ERROR, new_outage);
            }
        } catch (...) {
            lose(count);
            diagnostic("A queued log record could not be formatted", ERROR_INVALID_DATA);
        }
        return false;
    }

    // Flushes pending warning or error repetitions with the most recent repeat's timestamp.
    void summarize_problem() noexcept {
        const auto count = std::exchange(problem_repetitions_, 0);
        if (!count) {
            return;
        }

        try {
            const auto summary = std::format("Repeated {} more time{}: {}", count, count == 1 ? "" : "s", problem_text_);
            write_line(*problem_, summary, count);
        } catch (...) {
            lose(count);
            diagnostic("Repeated log records could not be summarized", ERROR_NOT_ENOUGH_MEMORY);
        }
    }

    // Formats one copied packet and writes it or folds an identical consecutive problem into the pending summary.
    void output(Record record, std::span<const std::byte> packet) noexcept {
        try {
            // Format on this worker so author threads only copy packet bytes into the queue.
            std::string message;
            if (record.formatter) {
                if (!record.formatter({reinterpret_cast<const std::uint8_t *>(packet.data()), static_cast<std::uint32_t>(packet.size())},
                                      {&message, &append_formatted})) {
                    lose();
                    diagnostic("The formatter for a queued log record failed", ERROR_INVALID_DATA);
                    return;
                }
            } else {
                message.assign(reinterpret_cast<const char *>(packet.data()), packet.size());
            }

            const bool problem = record.level == FC_LOG_WARNING || record.level == FC_LOG_ERROR;
            // Only consecutive identical problems share a summary; ownership and severity remain distinct.
            if (problem && problem_ && record.owner == problem_->owner && record.level == problem_->level && message == problem_text_) {
                ++problem_repetitions_;
                problem_->timestamp = record.timestamp;
                return;
            }

            summarize_problem();
            problem_.reset();
            problem_text_.clear();
            if (write_line(record, message, 1) && problem) {
                problem_ = record;
                problem_text_ = std::move(message);
            }
        } catch (...) {
            lose();
            diagnostic("A queued log record could not be formatted", ERROR_INVALID_DATA);
        }
    }

    // Reports new loss totals and diagnostics, retrying failed file output even when producers cannot enqueue.
    void report_diagnostics() noexcept {
        const auto lost = dropped_.load();
        bool changed{};
        std::array<char, 768> detail{};
        DWORD code{};
        FC_LogLevel level{};
        {
            std::scoped_lock lock(diagnostic_mutex_);
            changed = std::exchange(diagnostic_pending_, false);
            detail = diagnostic_text_;
            code = diagnostic_code_;
            level = diagnostic_level_;
        }
        const bool output_failed = failed_.load();
        if (lost == reported_drops_ && !changed && !output_failed) {
            return;
        }

        try {
            // Keep independent loss totals and diagnostic reasons at their own severities.
            const auto write_diagnostic = [this](std::string_view message, FC_LogLevel severity) {
                if (!enabled(severity)) {
                    return true;
                }
                const Record record{
                    nullptr, nullptr,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(), 0,
                    severity};
                DWORD output_code{};
                if (writer_->write(format_line(record, message), output_code)) {
                    if (failed_.exchange(false) && enabled(FC_LOG_INFO)) {
                        auto recovery = record;
                        recovery.level = FC_LOG_INFO;
                        if (!writer_->write(format_line(recovery, "FusionCutter.log output recovered"), output_code)) {
                            const bool new_outage = !failed_.exchange(true);
                            diagnostic("Could not write the FusionCutter.log recovery message", output_code, FC_LOG_ERROR, new_outage);
                        }
                    }
                    return true;
                } else {
                    const bool new_outage = !failed_.exchange(true);
                    diagnostic("Could not write reporting diagnostics to FusionCutter.log", output_code, FC_LOG_ERROR, new_outage);
                    return false;
                }
            };
            if ((changed || output_failed) && detail[0] && enabled(level)) {
                write_diagnostic(runtime::diagnostic_text({{}, detail.data(), code, level}), level);
            } else if (output_failed) {
                // A filtered latest explanation cannot prevent retries of a known failed file.
                write_diagnostic("FusionCutter.log output failed", FC_LOG_ERROR);
            }
            if (lost != reported_drops_) {
                const auto message = std::format("Dropped log records: {}", lost);
                if (lost != fallback_drops_ && enabled(FC_LOG_WARNING)) {
                    fallback(message, 0, FC_LOG_WARNING);
                    fallback_drops_ = lost;
                }
                if (write_diagnostic(message, FC_LOG_WARNING)) {
                    reported_drops_ = lost;
                }
            }
        } catch (...) {
            fallback("Reporting diagnostics could not be formatted", code, FC_LOG_ERROR);
        }
    }

    // Processes log batches, repetition summaries, status retries and diagnostics until shutdown drains the queue.
    void run() noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::array<std::byte, kRecordBytes - sizeof(Record)> packet{};
        SLIST_ENTRY *batch_head{};
        auto next_summary = std::chrono::steady_clock::now() + std::chrono::seconds{1};

        for (;;) {
            WaitForSingleObject(wake_, 1000);

            // A bounded pass gives status and independent diagnostics an opportunity between log batches.
            for (unsigned batch = 0; batch != 128; ++batch) {
                if (!batch_head) {
                    auto entry = InterlockedFlushSList(&pending_);
                    // Reverse the detached stack into publication order before adopting any later batch.
                    while (entry) {
                        auto next = entry->Next;
                        entry->Next = batch_head;
                        batch_head = entry;
                        entry = next;
                    }
                }
                if (!batch_head) {
                    break;
                }
                auto &slot = *reinterpret_cast<RecordSlot *>(batch_head);
                batch_head = batch_head->Next;
                const auto record = slot.record;
                std::memcpy(packet.data(), slot.payload.data(), record.payload_size);
                // Return storage before deferred formatting, so a stalled formatter does not retain a queue slot.
                InterlockedPushEntrySList(&free_, &slot.entry);
                output(record, std::span{packet}.first(record.payload_size));
            }

            // Elapsed time provides a summary opportunity even when accepted traffic keeps the event signaled.
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_summary) {
                summarize_problem();
                next_summary = now + std::chrono::seconds{1};
            }

            // Adopt the newest report, replacing any older content awaiting a successful write.
            {
                std::scoped_lock lock(status_mutex_);
                if (pending_status_) {
                    retry_status_.swap(pending_status_);
                }
                pending_status_.reset();
            }
            if (retry_status_) {
                const auto result = publisher_->publish(*retry_status_);
                if (result.outcome == StatusPublishOutcome::Failed) {
                    if (!status_failed_) {
                        diagnostic(result.failure, result.windows_error);
                    }
                    status_failed_ = true;
                } else {
                    retry_status_.reset();
                    if (status_failed_) {
                        status_failed_ = false;
                        diagnostic("FusionCutter.txt output recovered", 0, FC_LOG_INFO);
                    }
                }
            }

            report_diagnostics();

            // Drain queued logs during shutdown; wake immediately if another batch is already waiting.
            bool finished{};
            {
                std::scoped_lock lock(status_mutex_);
                // Observe stop and admitted producers before the final queue check so a late publication cannot be missed.
                const bool stopped = admission_closed_.load() && active_writes_.load() == 0;
                const bool queued = batch_head || QueryDepthSList(&pending_) != 0;
                finished = stopped && !queued && !pending_status_;
                if (queued || pending_status_) {
                    SetEvent(wake_);
                }
            }
            if (finished) {
                summarize_problem();
                report_diagnostics();
                return;
            }
        }
    }

    std::unique_ptr<RecordSlot[]> slots_;
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER free_{}, pending_{};
    std::mutex status_mutex_, diagnostic_mutex_, shutdown_mutex_;
    std::optional<std::string> pending_status_;
    // The worker retries a failed snapshot at later opportunities; a newer collected snapshot replaces it.
    std::optional<std::string> retry_status_;
    // Last warning or error used to combine consecutive repeats into a summary.
    std::optional<Record> problem_;
    std::string problem_text_;
    std::uint64_t problem_repetitions_{};
    std::unique_ptr<StatusPublisher> publisher_;
    std::unique_ptr<LogWriter> writer_;
    std::filesystem::path path_;
    HANDLE wake_{};
    std::thread worker_;
    std::atomic_bool ready_{}, failed_{};
    std::atomic<std::uint32_t> active_writes_{};
    std::atomic<std::uint64_t> accepted_{}, written_{}, dropped_{};

    // Protected by diagnostic_mutex_; a producer after the worker snapshot marks the next pass pending.
    bool diagnostic_pending_{};
    std::array<char, 768> diagnostic_text_{};
    DWORD diagnostic_code_{};
    FC_LogLevel diagnostic_level_{FC_LOG_ERROR};
    std::uint64_t reported_drops_{}, fallback_drops_{};
    bool status_failed_{};
};

ReportingSession::ReportingSession() : state_(std::make_unique<State>()) {}

ReportingSession::~ReportingSession() = default;

void ReportingSession::shutdown() noexcept {
    state_->shutdown();
}

void ReportingSession::start(SessionFacts facts, std::filesystem::path path) noexcept {
    state_->start(std::move(facts), std::move(path));
}

void ReportingSession::configure(FC_LogLevel level) noexcept {
    state_->level_.store(level);
}

bool ReportingSession::enabled(FC_LogLevel level) const noexcept {
    return !state_->admission_closed_.load() && state_->enabled(level);
}

bool ReportingSession::submit(const runtime::PatchRecord *owner, FC_LogLevel level, FC_LogPacket packet) noexcept {
    if (!packet.format) {
        return false;
    }
    return state_->enqueue(owner, level, packet.copied, packet.format);
}

bool ReportingSession::write(const runtime::PatchRecord *owner, FC_LogLevel level, FC_Text text) noexcept {
    return state_->enqueue(owner, level, {reinterpret_cast<const std::uint8_t *>(text.data), text.size}, nullptr);
}

void ReportingSession::publish_status(std::string content) noexcept {
    state_->publish(std::move(content));
}

LogStatus ReportingSession::status() const noexcept {
    return state_->status();
}

void ReportingSession::diagnostic(std::string_view reason, std::uint32_t code, FC_LogLevel level) noexcept {
    state_->submit_diagnostic(reason, code, level);
}

} // namespace fc::reporting
