#include "session.hpp"

#include "status.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/Sink.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fusioncutter::reporting {
namespace {

constexpr std::uintmax_t kMaximumLogFileSize = 4 * 1024 * 1024;
constexpr std::size_t kMaximumLogLineSize = 8 * 1024;
constexpr std::wstring_view kClientLogFilename = L"FusionCutter.log";
constexpr std::wstring_view kServerLogFilename = L"FusionCutter-Server.log";
constexpr std::wstring_view kClientStatusFilename = L"FusionCutter.txt";
constexpr std::wstring_view kServerStatusFilename = L"FusionCutter-Server.txt";
const int kModuleAnchor{};

struct SupportPaths {
    std::filesystem::path log;
    std::filesystem::path status;
};

struct FrontendOptions : quill::FrontendOptions {
    static constexpr auto queue_type = quill::QueueType::BoundedDropping;
    static constexpr std::size_t initial_queue_capacity = 128 * 1024;
};

using Frontend = quill::FrontendImpl<FrontendOptions>;
using Logger = quill::LoggerImpl<FrontendOptions>;

void debug_output(std::string_view message, DWORD error = ERROR_SUCCESS) noexcept {
    std::string text = "Fusion Cutter reporting: ";
    text.append(message);
    if (error != ERROR_SUCCESS) {
        text.append(" (Windows error ");
        text.append(std::to_string(error));
        text.push_back(')');
    }
    text.append("\r\n");
    OutputDebugStringA(text.c_str());
}

[[nodiscard]] std::optional<SupportPaths> support_paths(HostRole role) noexcept {
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&kModuleAnchor), &owner) ||
        owner == nullptr) {
        debug_output("the owning module is unavailable", GetLastError());
        return std::nullopt;
    }

    std::wstring module_path(32'768, L'\0');
    const auto path_length = GetModuleFileNameW(owner, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (path_length == 0 || path_length >= module_path.size()) {
        debug_output("the owning module path is unavailable",
                     path_length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
        return std::nullopt;
    }
    module_path.resize(path_length);

    auto directory = std::filesystem::path(std::move(module_path)).parent_path();
    return SupportPaths{
        directory / (role == HostRole::Client ? kClientLogFilename : kServerLogFilename),
        directory / (role == HostRole::Client ? kClientStatusFilename : kServerStatusFilename),
    };
}

[[nodiscard]] std::filesystem::path backup_path(const std::filesystem::path& path, int generation) {
    auto backup = path;
    backup.replace_filename(path.stem().wstring() + L"." + std::to_wstring(generation) + path.extension().wstring());
    return backup;
}

[[nodiscard]] bool move_if_present(const std::filesystem::path& source,
                                   const std::filesystem::path& destination) noexcept {
    const auto attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

[[nodiscard]] std::string one_line(std::string_view statement) {
    constexpr std::string_view kMarker = " [truncated]";
    const auto content_capacity = kMaximumLogLineSize - 2;
    const bool truncated = statement.size() > content_capacity;
    const auto copied_size = truncated ? content_capacity - kMarker.size() : statement.size();

    std::string output(statement.substr(0, copied_size));
    std::ranges::replace_if(
        output,
        [](char character) {
            return character == '\r' || character == '\n';
        },
        ' ');
    if (truncated) {
        output.append(kMarker);
    }
    output.append("\r\n");
    return output;
}

[[nodiscard]] std::string drop_summary(std::uint64_t count) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [Warning] [Core] Logging queue dropped {} records",
                       time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                       count);
}

[[nodiscard]] std::uint64_t dropped_count(std::string_view notification) noexcept {
    constexpr std::string_view kPrefix = "Dropped ";
    const auto position = notification.find(kPrefix);
    if (position == std::string_view::npos) {
        return 0;
    }

    const auto number = notification.substr(position + kPrefix.size());
    std::uint64_t count = 0;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), count);
    return parsed.ec == std::errc{} ? count : 0;
}

[[nodiscard]] quill::LogLevel quill_level(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Off:
        return quill::LogLevel::None;
    case LogLevel::Error:
        return quill::LogLevel::Error;
    case LogLevel::Warning:
        return quill::LogLevel::Warning;
    case LogLevel::Info:
        return quill::LogLevel::Info;
    case LogLevel::Debug:
        return quill::LogLevel::Debug;
    }
    return quill::LogLevel::None;
}

[[nodiscard]] bool accepts(LogLevel threshold, LogLevel entry) noexcept {
    return entry != LogLevel::Off && threshold != LogLevel::Off &&
           std::to_underlying(entry) <= std::to_underlying(threshold);
}

class LogWriter {
  public:
    LogWriter(std::filesystem::path path, HostRole role, std::atomic_uint64_t& written_entries,
              std::atomic_bool& output_failed)
        : path_(std::move(path)), backup_one_(backup_path(path_, 1)), backup_two_(backup_path(path_, 2)), role_(role),
          written_entries_(written_entries), output_failed_(output_failed) {}

    void write(std::string_view statement, bool count_entry = true) noexcept {
        try {
            auto line = one_line(statement);
            const auto header = session_header();
            if (!ensure_open(line.size() + (header_pending_ ? header.size() : 0))) {
                return;
            }
            if (header_pending_) {
                output_.write(header.data(), static_cast<std::streamsize>(header.size()));
                current_size_ += header.size();
                header_pending_ = false;
            }
            output_.write(line.data(), static_cast<std::streamsize>(line.size()));
            if (!output_) {
                fail("the log could not be written", GetLastError());
                return;
            }
            current_size_ += line.size();
            if (count_entry) {
                written_entries_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            fail("an unexpected log file error occurred", ERROR_GEN_FAILURE);
        }
    }

    void flush() noexcept {
        if (!output_.is_open()) {
            return;
        }
        output_.flush();
        if (!output_) {
            fail("the log could not be flushed", GetLastError());
        }
    }

  private:
    [[nodiscard]] std::string session_header() const {
        return std::format("--- Fusion Cutter {} (build {}, {}) ---\r\n", FC_VERSION_STRING, FC_BUILD_ID,
                           role_ == HostRole::Client ? "Client" : "Server");
    }

    [[nodiscard]] bool ensure_open(std::size_t next_write_size) noexcept {
        if (output_failed_.load(std::memory_order_relaxed)) {
            return false;
        }

        if (!output_.is_open()) {
            std::error_code error;
            current_size_ =
                std::filesystem::exists(path_, error) && !error ? std::filesystem::file_size(path_, error) : 0;
            if (error) {
                fail("the existing log size could not be inspected", static_cast<DWORD>(error.value()));
                return false;
            }
        }

        if (current_size_ + next_write_size > kMaximumLogFileSize && !rotate()) {
            return false;
        }
        if (output_.is_open()) {
            return true;
        }

        output_.open(path_, std::ios::binary | std::ios::app);
        if (!output_) {
            fail("the log could not be opened", GetLastError());
            return false;
        }
        header_pending_ = true;
        return true;
    }

    [[nodiscard]] bool rotate() noexcept {
        if (output_.is_open()) {
            output_.close();
            if (!output_) {
                fail("the log could not be closed for rotation", GetLastError());
                return false;
            }
        }

        if (!move_if_present(backup_one_, backup_two_) || !move_if_present(path_, backup_one_)) {
            fail("the log could not be rotated", GetLastError());
            return false;
        }
        current_size_ = 0;
        header_pending_ = true;
        return true;
    }

    void fail(std::string_view message, DWORD error) noexcept {
        output_failed_.store(true, std::memory_order_relaxed);
        debug_output(message, error);
        if (output_.is_open()) {
            output_.close();
        }
    }

    std::filesystem::path path_;
    std::filesystem::path backup_one_;
    std::filesystem::path backup_two_;
    HostRole role_;
    std::ofstream output_;
    std::uintmax_t current_size_{};
    bool header_pending_{true};
    std::atomic_uint64_t& written_entries_;
    std::atomic_bool& output_failed_;
};

} // namespace

class Session::State {
  public:
    class Sink final : public quill::Sink {
      public:
        explicit Sink(State& owner) : owner_(owner) {}

      private:
        void write_log(const quill::MacroMetadata*, std::uint64_t, std::string_view, std::string_view,
                       const std::string&, std::string_view, quill::LogLevel, std::string_view, std::string_view,
                       const std::vector<std::pair<std::string, std::string>>*, std::string_view,
                       std::string_view log_statement) override {
            owner_.writer_->write(log_statement);
        }

        void flush_sink() override {
            owner_.writer_->flush();
        }

        void run_periodic_tasks() override {
            owner_.run_periodic_tasks();
        }

        State& owner_;
    };

    void start(HostRole requested_role) noexcept {
        try {
            start_once(requested_role);
        } catch (...) {
            output_failed_.store(true, std::memory_order_relaxed);
            debug_output("the reporting session could not be initialized", ERROR_GEN_FAILURE);
        }
    }

    void start_once(HostRole requested_role) {
        const std::scoped_lock lock(start_mutex_);
        if (started_) {
            return;
        }
        started_ = true;
        role_ = requested_role;
        level_.store(compiled_default_log_level(), std::memory_order_relaxed);

        const auto paths = support_paths(role_);
        if (!paths.has_value()) {
            output_failed_.store(true, std::memory_order_relaxed);
            return;
        }
        log_path_ = paths->log;
        status_ = std::make_unique<StatusPublisher>(paths->status);
        writer_ = std::make_unique<LogWriter>(log_path_, role_, written_entries_, output_failed_);

        try {
            quill::BackendOptions options;
            options.transit_event_buffer_initial_capacity = 256;
            options.transit_events_soft_limit = 1024;
            options.transit_events_hard_limit = 8192;
            options.error_notifier = [this](const std::string& notification) {
                const auto count = dropped_count(notification);
                if (count != 0) {
                    dropped_entries_.fetch_add(count, std::memory_order_relaxed);
                } else {
                    debug_output(notification);
                }
            };
            quill::Backend::start(options);

            sink_ = Frontend::create_or_get_sink<Sink>("fusioncutter-reporting-sink", *this);
            auto* logger = Frontend::create_or_get_logger(
                "fusioncutter", sink_,
                quill::PatternFormatterOptions{"%(time) [%(log_level)] %(message)", "%Y-%m-%d %H:%M:%S.%Qms",
                                               quill::Timezone::LocalTime});
            logger->set_log_level(quill_level(level_.load(std::memory_order_relaxed)));
            logger_.store(logger, std::memory_order_release);
        } catch (...) {
            output_failed_.store(true, std::memory_order_relaxed);
            debug_output("the Quill backend could not be initialized", ERROR_GEN_FAILURE);
        }
    }

    [[nodiscard]] LogStatus log_status() const noexcept {
        return {
            level_.load(std::memory_order_relaxed),
            log_path_,
            accepted_entries_.load(std::memory_order_relaxed),
            written_entries_.load(std::memory_order_relaxed),
            output_failed_.load(std::memory_order_relaxed),
        };
    }

    void run_periodic_tasks() noexcept {
        try {
            const auto dropped = dropped_entries_.load(std::memory_order_relaxed);
            if (dropped != reported_drops_) {
                writer_->write(drop_summary(dropped - reported_drops_), false);
                reported_drops_ = dropped;
            }
            if (status_ != nullptr) {
                status_->publish_if_due(log_status());
            }
        } catch (...) {
            debug_output("a periodic reporting task failed", ERROR_GEN_FAILURE);
        }
    }

    std::mutex start_mutex_;
    bool started_{};
    HostRole role_{HostRole::Client};
    std::atomic<LogLevel> level_{compiled_default_log_level()};
    std::filesystem::path log_path_;
    std::unique_ptr<LogWriter> writer_;
    std::unique_ptr<StatusPublisher> status_;
    std::shared_ptr<quill::Sink> sink_;
    std::atomic<Logger*> logger_{};
    std::atomic_uint64_t accepted_entries_{};
    std::atomic_uint64_t written_entries_{};
    std::atomic_uint64_t dropped_entries_{};
    std::atomic_bool output_failed_{};
    std::uint64_t reported_drops_{};
};

Session::Session() : state_(new State) {}

Session& Session::instance() noexcept {
    static auto* session = new Session;
    return *session;
}

void Session::start(HostRole role) noexcept {
    state_->start(role);
}

void Session::set_level(LogLevel level) noexcept {
    try {
        state_->level_.store(level, std::memory_order_relaxed);
        if (auto* logger = state_->logger_.load(std::memory_order_acquire); logger != nullptr) {
            logger->set_log_level(quill_level(level));
        }
    } catch (...) {
        debug_output("the logging level could not be applied", ERROR_GEN_FAILURE);
    }
}

void Session::set_target(const TargetContext& target) noexcept {
    try {
        if (state_->status_ != nullptr) {
            state_->status_->set_target(target);
        }
    } catch (...) {
        debug_output("the recognized target could not be recorded", ERROR_GEN_FAILURE);
    }
}

void Session::set_configuration(const std::filesystem::path& path) noexcept {
    try {
        if (state_->status_ != nullptr) {
            state_->status_->set_configuration(path);
        }
    } catch (...) {
        debug_output("the configuration path could not be recorded", ERROR_GEN_FAILURE);
    }
}

void Session::publish_status(const InitializationResult& initialization, std::span<const PatchResult> patch_results,
                             std::span<const StatusContributorRef> contributors) noexcept {
    try {
        if (state_->status_ == nullptr) {
            return;
        }
        state_->status_->set_snapshot(initialization, patch_results, contributors);
        state_->status_->publish_now(state_->log_status());
    } catch (...) {
        debug_output("the initialization status could not be published", ERROR_GEN_FAILURE);
    }
}

void Session::flush() noexcept {
    try {
        if (auto* logger = state_->logger_.load(std::memory_order_acquire); logger != nullptr) {
            logger->flush_log();
        }
    } catch (...) {
        state_->output_failed_.store(true, std::memory_order_relaxed);
        debug_output("the ordinary log could not be flushed", ERROR_GEN_FAILURE);
    }
}

bool Session::enabled(LogLevel level) const noexcept {
    return accepts(state_->level_.load(std::memory_order_relaxed), level) &&
           state_->logger_.load(std::memory_order_acquire) != nullptr;
}

void Session::write(LogLevel level, PatchId source, std::string_view message, std::string_view operation,
                    PatchId related_patch) noexcept {
    try {
        if (!accepts(state_->level_.load(std::memory_order_relaxed), level)) {
            return;
        }
        auto* logger = state_->logger_.load(std::memory_order_acquire);
        if (logger == nullptr) {
            return;
        }

        state_->accepted_entries_.fetch_add(1, std::memory_order_relaxed);
        const auto normalized_source = source.empty() ? std::string_view{"Core"} : source;
        const auto operation_label = operation.empty() ? std::string_view{} : std::string_view{" | Operation: "};
        const auto related_label = related_patch.empty() ? std::string_view{} : std::string_view{" | Related patch: "};
        QUILL_LOG_DYNAMIC(logger, quill_level(level), "[{}] {}{}{}{}{}", normalized_source, message, operation_label,
                          operation, related_label, related_patch);
    } catch (...) {
        debug_output("an ordinary log entry could not be queued", ERROR_GEN_FAILURE);
    }
}

} // namespace fusioncutter::reporting

namespace fusioncutter::logging {

bool enabled(LogLevel level) noexcept {
    return reporting::Session::instance().enabled(level);
}

void write(LogLevel level, PatchId source, std::string_view message, std::string_view operation,
           PatchId related_patch) noexcept {
    reporting::Session::instance().write(level, source, message, operation, related_patch);
}

} // namespace fusioncutter::logging
