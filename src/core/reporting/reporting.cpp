#include "reporting.hpp"

#include "../runtime/patch_runtime.hpp"
#include "tracing.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/Sink.h>

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace fc::reporting {
namespace {

inline constexpr std::uintmax_t kMaximumLogFileBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kAggregateQueueBytes = 2U * 1024U * 1024U;
inline constexpr std::size_t kEncodedRecordOverhead = 24;
inline constexpr std::string_view kReservationMarker = "|FCQ|";
inline constexpr auto kFatalFlushBudget = std::chrono::milliseconds{250};

// Quill supplies the reviewed bounded producer transport; Fusion Cutter owns formatting, files, and status coupling.
struct FrontendOptions : quill::FrontendOptions {
    static constexpr auto queue_type = quill::QueueType::BoundedDropping;
    // The framework's shared reservation is authoritative; extra headroom prevents Quill's per-thread rings from
    // imposing an accidental second, smaller aggregate policy once a record has consumed that reservation.
    static constexpr std::size_t initial_queue_capacity = 2 * kAggregateQueueBytes;
};

using Frontend = quill::FrontendImpl<FrontendOptions>;
using BackendLogger = quill::LoggerImpl<FrontendOptions>;

[[nodiscard]] quill::LogLevel quill_level(FC_LogLevel level) noexcept {
    switch (level) {
    case FC_LOG_ERROR:
        return quill::LogLevel::Error;
    case FC_LOG_WARNING:
        return quill::LogLevel::Warning;
    case FC_LOG_INFO:
        return quill::LogLevel::Info;
    case FC_LOG_DEBUG:
        return quill::LogLevel::Debug;
    default:
        return quill::LogLevel::None;
    }
}

[[nodiscard]] bool accepts(FC_LogLevel threshold, FC_LogLevel entry) noexcept {
    return entry >= FC_LOG_ERROR && entry <= FC_LOG_DEBUG && threshold >= FC_LOG_ERROR && threshold <= FC_LOG_DEBUG &&
           entry <= threshold;
}

void debug_output(std::string_view message, DWORD error = ERROR_SUCCESS) noexcept {
    try {
        std::array<char, 768> output{};
        const auto rendered =
            std::format_to_n(output.data(), output.size() - 1, "Fusion Cutter reporting: {}{}\r\n", message,
                             error == ERROR_SUCCESS ? std::string{} : std::format(" (Windows error {})", error));
        output[std::min<std::size_t>(rendered.size, output.size() - 1)] = '\0';
        OutputDebugStringA(output.data());
    } catch (...) {
        // OutputDebugString is the final reporting fallback, so formatting failure must not escape this noexcept path.
        OutputDebugStringA("Fusion Cutter reporting failed while producing a diagnostic.\r\n");
    }
}

[[nodiscard]] std::optional<std::filesystem::path> module_directory() {
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&module_directory), &module) == 0 ||
        module == nullptr) {
        return std::nullopt;
    }
    std::array<wchar_t, 32'768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return std::nullopt;
    }
    return std::filesystem::path{std::wstring_view{path.data(), length}}.parent_path();
}

[[nodiscard]] std::string executable_basename() {
    std::array<wchar_t, 32'768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return "Unknown";
    }
    const auto basename = std::filesystem::path{std::wstring_view{path.data(), length}}.filename().wstring();
    if (basename.empty()) {
        return "Unknown";
    }
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, basename.data(),
                                              static_cast<int>(basename.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return "Unknown";
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, basename.data(), static_cast<int>(basename.size()),
                            result.data(), required, nullptr, nullptr) != required) {
        return "Unknown";
    }
    return result;
}

// Status for unsupported targets retains a compact executable fingerprint without invoking target recognition.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint64_t>> executable_fingerprint() noexcept {
    const auto module = GetModuleHandleW(nullptr);
    MODULEINFO information{};
    if (module == nullptr ||
        K32GetModuleInformation(GetCurrentProcess(), module, &information, sizeof(information)) == 0 ||
        information.lpBaseOfDll == nullptr || information.SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        return std::nullopt;
    }
    const auto image = std::span{static_cast<const std::byte*>(information.lpBaseOfDll),
                                 static_cast<std::size_t>(information.SizeOfImage)};
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return std::nullopt;
    }
    const auto file_offset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD);
    if (file_offset > image.size() || sizeof(IMAGE_FILE_HEADER) > image.size() - file_offset) {
        return std::nullopt;
    }
    IMAGE_FILE_HEADER file{};
    std::memcpy(&file, image.data() + file_offset, sizeof(file));
    return std::pair{file.TimeDateStamp, static_cast<std::uint64_t>(information.SizeOfImage)};
}

[[nodiscard]] std::filesystem::path backup_path(const std::filesystem::path& path, unsigned generation) {
    auto result = path;
    result.replace_filename(path.stem().wstring() + L"." + std::to_wstring(generation) + path.extension().wstring());
    return result;
}

[[nodiscard]] bool move_if_present(const std::filesystem::path& source,
                                   const std::filesystem::path& destination) noexcept {
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

[[nodiscard]] std::uint64_t dropped_count(std::string_view notification) noexcept {
    constexpr std::string_view prefix = "Dropped ";
    const auto position = notification.find(prefix);
    if (position == std::string_view::npos) {
        return 0;
    }
    const auto number = notification.substr(position + prefix.size());
    std::uint64_t result{};
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result);
    return parsed.ec == std::errc{} ? result : 0;
}

[[nodiscard]] std::string utc_started(std::chrono::system_clock::time_point started) {
    const auto time = std::chrono::system_clock::to_time_t(started);
    std::tm value{};
    gmtime_s(&value, &time);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}Z", value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                       value.tm_hour, value.tm_min, value.tm_sec);
}

// LogWriter is used only by the backend thread; its direct file policy is independent of producer queue coordination.
class LogWriter final {
  public:
    LogWriter(std::filesystem::path path, const SessionFacts& session, std::atomic_uint64_t& written,
              std::atomic_bool& failed)
        : path_(std::move(path)), backup_one_(backup_path(path_, 1)), backup_two_(backup_path(path_, 2)),
          header_(std::format("--- Fusion Cutter {} (build {}) ---\r\nUTC start: {}\r\nPID: {}\r\nRole: {}\r\n"
                              "Architecture: {}\r\nExecutable: {}\r\n---\r\n",
                              FC_VERSION_STRING, FC_BUILD_ID, utc_started(session.started), GetCurrentProcessId(),
                              session.role == FC_HOST_ROLE_SERVER ? "Server" : "Client",
                              sizeof(void*) == 8 ? "x64" : "x86", session.executable_basename)),
          written_(written), failed_(failed) {}

    void write(std::string_view statement, bool ordinary = true) noexcept {
        try {
            // Rotation is decided from the complete pending record so neither the session header nor a record is split.
            const auto next_size = statement.size() + 2 + (header_pending_ ? header_.size() : 0);
            if (!ensure_open(next_size)) {
                return;
            }
            // Each newly created generation begins with enough session identity to stand alone after rotation.
            if (header_pending_) {
                output_.write(header_.data(), static_cast<std::streamsize>(header_.size()));
                current_size_ += header_.size();
                header_pending_ = false;
            }
            output_.write(statement.data(), static_cast<std::streamsize>(statement.size()));
            output_.write("\r\n", 2);
            if (!output_) {
                fail("FusionCutter.log could not be written", GetLastError());
                return;
            }
            current_size_ += statement.size() + 2;
            if (ordinary) {
                written_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            fail("an unexpected log file operation failed", ERROR_GEN_FAILURE);
        }
    }

    void flush() noexcept {
        if (!output_.is_open()) {
            return;
        }
        output_.flush();
        if (!output_) {
            fail("FusionCutter.log could not be flushed", GetLastError());
        }
    }

  private:
    [[nodiscard]] bool ensure_open(std::size_t next_size) noexcept {
        if (failed_.load(std::memory_order_relaxed)) {
            return false;
        }
        // Existing size is inspected lazily because a quiet session must not create or otherwise touch the log.
        if (!inspected_) {
            inspected_ = true;
            std::error_code error;
            current_size_ =
                std::filesystem::exists(path_, error) && !error ? std::filesystem::file_size(path_, error) : 0;
            if (error) {
                fail("the existing log size could not be inspected", static_cast<DWORD>(error.value()));
                return false;
            }
        }
        // Rotate before opening for append when the next complete record would cross the per-file bound.
        if (current_size_ != 0 && next_size > kMaximumLogFileBytes - std::min(current_size_, kMaximumLogFileBytes) &&
            !rotate()) {
            return false;
        }
        if (output_.is_open()) {
            return true;
        }
        output_.open(path_, std::ios::binary | std::ios::app);
        if (!output_) {
            fail("FusionCutter.log could not be opened", GetLastError());
            return false;
        }
        header_pending_ = true;
        return true;
    }

    [[nodiscard]] bool rotate() noexcept {
        if (output_.is_open()) {
            output_.close();
            if (!output_) {
                fail("FusionCutter.log could not close for rotation", GetLastError());
                return false;
            }
        }
        if (!move_if_present(backup_one_, backup_two_) || !move_if_present(path_, backup_one_)) {
            fail("FusionCutter.log rotation failed", GetLastError());
            return false;
        }
        current_size_ = 0;
        header_pending_ = true;
        return true;
    }

    void fail(std::string_view message, DWORD error) noexcept {
        failed_.store(true, std::memory_order_relaxed);
        debug_output(message, error);
        if (output_.is_open()) {
            output_.close();
        }
    }

    std::filesystem::path path_;
    std::filesystem::path backup_one_;
    std::filesystem::path backup_two_;
    std::string header_;
    std::ofstream output_;
    std::uintmax_t current_size_{};
    bool inspected_{};
    bool header_pending_{true};
    std::atomic_uint64_t& written_;
    std::atomic_bool& failed_;
};

} // namespace

// State owns the asynchronous logger, its backend-only file sink, and the latest status inputs as one lifetime unit.
class ReportingSession::State final {
  public:
    class Sink final : public quill::Sink {
      public:
        explicit Sink(State& owner) : owner_(&owner) {}

      private:
        void write_log(const quill::MacroMetadata*, std::uint64_t, std::string_view, std::string_view,
                       const std::string&, std::string_view, quill::LogLevel, std::string_view, std::string_view,
                       const std::vector<std::pair<std::string, std::string>>*, std::string_view,
                       std::string_view statement) override {
            // The private suffix returns the producer's exact aggregate byte reservation after backend consumption.
            const auto marker = statement.rfind(kReservationMarker);
            std::uint64_t reservation{};
            if (marker == std::string_view::npos || marker + kReservationMarker.size() + 16 > statement.size() ||
                std::from_chars(statement.data() + marker + kReservationMarker.size(),
                                statement.data() + marker + kReservationMarker.size() + 16, reservation, 16)
                        .ec != std::errc{}) {
                debug_output("an internally encoded log reservation was malformed", ERROR_INVALID_DATA);
                return;
            }
            owner_->writer_->write(statement.substr(0, marker));
            owner_->release_reservation(static_cast<std::size_t>(reservation));
        }

        void flush_sink() override {
            owner_->writer_->flush();
            owner_->flush_generation_.fetch_add(1, std::memory_order_release);
        }

        void run_periodic_tasks() override {
            owner_->report_drops();
        }

        State* owner_{};
    };

    ~State() {
        // Tests and tools have finite owners; remove the unique logger before its sink's raw State pointer expires.
        if (auto* backend = logger_.exchange(nullptr, std::memory_order_acq_rel); backend != nullptr) {
            try {
                Frontend::remove_logger_blocking(backend);
            } catch (...) {
                debug_output("the reporting logger could not be removed cleanly", ERROR_GEN_FAILURE);
            }
        }
        sink_.reset();
    }

    void start(SessionFacts facts, std::filesystem::path requested_directory) noexcept {
        try {
            // Only the first caller publishes session-wide output identities and starts the shared Quill backend.
            const std::scoped_lock lock(start_mutex_);
            if (started_) {
                return;
            }
            started_ = true;
            if (facts.executable_basename.empty()) {
                facts.executable_basename = executable_basename();
            }
            if (!facts.executable_timestamp || !facts.executable_image_size) {
                if (const auto fingerprint = executable_fingerprint(); fingerprint) {
                    facts.executable_timestamp = fingerprint->first;
                    facts.executable_image_size = fingerprint->second;
                }
            }
            session_ = facts;
            // Production output is adjacent to the framework DLL; tests may provide an isolated directory explicitly.
            const auto directory =
                requested_directory.empty() ? module_directory() : std::optional{std::move(requested_directory)};
            if (!directory) {
                output_failed_.store(true, std::memory_order_relaxed);
                debug_output("the FusionCutter.dll directory is unavailable", GetLastError());
                return;
            }
            log_path_ = *directory / "FusionCutter.log";
            status_ = std::make_unique<StatusPublisher>(*directory / "FusionCutter.txt");
            status_->set_session(facts);
            writer_ = std::make_unique<LogWriter>(log_path_, facts, written_, output_failed_);

            // The asynchronous transport starts only after all objects reachable from its sink have stable owners.
            quill::BackendOptions options;
            options.transit_event_buffer_initial_capacity = 256;
            options.transit_events_soft_limit = 1024;
            options.transit_events_hard_limit = 8192;
            // Idle flushing lets fatal handling observe completion without Quill's unbounded blocking flush API.
            options.sink_min_flush_interval = std::chrono::milliseconds{0};
            options.error_notifier = [this](const std::string& notification) {
                const auto count = dropped_count(notification);
                if (count == 0) {
                    debug_output(notification);
                } else {
                    dropped_.fetch_add(count, std::memory_order_relaxed);
                }
            };
            quill::Backend::start(options);
            sink_ = Frontend::create_or_get_sink<Sink>(
                std::format("fusioncutter-reporting-sink-{}", reinterpret_cast<std::uintptr_t>(this)), *this);
            auto* backend = Frontend::create_or_get_logger(
                std::format("fusioncutter-{}", reinterpret_cast<std::uintptr_t>(this)), sink_,
                quill::PatternFormatterOptions{"%(time) [%(log_level)] %(message)", "%Y-%m-%d %H:%M:%S.%Qms",
                                               quill::Timezone::LocalTime, false});
            backend->set_log_level(quill_level(level_.load(std::memory_order_relaxed)));
            logger_.store(backend, std::memory_order_release);
        } catch (...) {
            output_failed_.store(true, std::memory_order_relaxed);
            debug_output("the reporting backend could not be initialized", ERROR_GEN_FAILURE);
        }
    }

    void configure(FC_LogLevel level) noexcept {
        if (level > FC_LOG_DEBUG) {
            return;
        }
        level_.store(level, std::memory_order_relaxed);
        if (auto* backend = logger_.load(std::memory_order_acquire); backend != nullptr) {
            backend->set_log_level(quill_level(level));
        }
    }

    [[nodiscard]] std::string source(FC_ReportToken report) const {
        // Tokens encode patch indices; invalid or unavailable values retain framework diagnostic attribution.
        if (report == nullptr || catalog_ == nullptr) {
            return "FusionCutter";
        }
        const auto encoded = reinterpret_cast<std::uintptr_t>(report);
        if (encoded == 0 || encoded - 1 > std::numeric_limits<std::uint32_t>::max()) {
            return "FusionCutter";
        }
        const catalog::PatchIndex patch{static_cast<std::uint32_t>(encoded - 1)};
        if (patch.value >= catalog_->patch_count()) {
            return "FusionCutter";
        }
        const auto& plugin = catalog_->plugin(catalog_->patch_plugin(patch));
        return plugin.definition.id + "/" + catalog_->patch(patch).id;
    }

    [[nodiscard]] LogStatus log_status() const noexcept {
        return {.level = level_.load(std::memory_order_relaxed),
                .path = log_path_,
                .accepted = accepted_.load(std::memory_order_relaxed),
                .written = written_.load(std::memory_order_relaxed),
                .dropped = dropped_.load(std::memory_order_relaxed),
                .output_failed = output_failed_.load(std::memory_order_relaxed)};
    }

    [[nodiscard]] bool enabled(FC_LogLevel level) const noexcept {
        return logger_.load(std::memory_order_acquire) != nullptr &&
               accepts(level_.load(std::memory_order_relaxed), level);
    }

    void enqueue(std::string_view source, FC_LogLevel level, FC_StringView message) noexcept {
        // Reject filtered or malformed input before attribution consumes bounded queue capacity.
        if (message.data == nullptr || message.size == 0 || !enabled(level)) {
            return;
        }
        try {
            auto* backend = logger_.load(std::memory_order_acquire);
            if (backend == nullptr) {
                return;
            }
            // Charge the logical record, including private framing, before Quill copies it into async transport.
            const auto message_size = static_cast<std::size_t>(message.size);
            if (source.size() > std::numeric_limits<std::size_t>::max() - kEncodedRecordOverhead ||
                message_size > std::numeric_limits<std::size_t>::max() - kEncodedRecordOverhead - source.size()) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto reservation = kEncodedRecordOverhead + source.size() + message_size;
            if (!reserve(reservation)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            try {
                // The sink strips this suffix and returns the exact process-wide reservation after consumption.
                QUILL_LOG_DYNAMIC(backend, quill_level(level), "[{}] {}{}{:016x}", source,
                                  std::string_view{message.data, message.size}, kReservationMarker, reservation);
                accepted_.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                release_reservation(reservation);
                throw;
            }
        } catch (...) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            debug_output("a scoped log record could not be queued", ERROR_GEN_FAILURE);
        }
    }

    [[nodiscard]] bool reserve(std::size_t bytes) noexcept {
        // This shared producer charge enforces one process-wide queue budget across Quill's per-thread rings.
        auto current = queued_bytes_.load(std::memory_order_relaxed);
        while (bytes <= kAggregateQueueBytes - std::min(current, kAggregateQueueBytes)) {
            if (queued_bytes_.compare_exchange_weak(current, current + bytes, std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void release_reservation(std::size_t bytes) noexcept {
        queued_bytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    }

    void report_drops() noexcept {
        // Drop summaries bypass the full producer queue so overload remains visible without worsening that overload.
        try {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_drop_report_ < std::chrono::seconds{1}) {
                return;
            }
            const auto total = dropped_.load(std::memory_order_relaxed);
            if (total == reported_drops_ || writer_ == nullptr) {
                return;
            }
            last_drop_report_ = now;
            writer_->write(std::format("{} [Warning] [FusionCutter] Logging queue dropped {} record(s)",
                                       std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::system_clock::now()),
                                       total - reported_drops_),
                           false);
            reported_drops_ = total;
        } catch (...) {
            output_failed_.store(true, std::memory_order_relaxed);
            debug_output("a summary of records dropped from the logging queue could not be produced",
                         ERROR_GEN_FAILURE);
        }
    }

    void flush() noexcept {
        try {
            if (logger_.load(std::memory_order_acquire) == nullptr) {
                return;
            }
            // Wake the backend once, then wait only through the explicit fatal budget for all accepted reservations and
            // a following sink flush. A wedged backend or file can never hold process termination indefinitely.
            const auto initial_generation = flush_generation_.load(std::memory_order_acquire);
            const auto deadline = std::chrono::steady_clock::now() + kFatalFlushBudget;
            quill::Backend::notify();
            while (std::chrono::steady_clock::now() < deadline) {
                if (queued_bytes_.load(std::memory_order_acquire) == 0 &&
                    flush_generation_.load(std::memory_order_acquire) != initial_generation) {
                    return;
                }
                Sleep(1);
            }
            debug_output("the bounded flush of accepted log records timed out", WAIT_TIMEOUT);
        } catch (...) {
            output_failed_.store(true, std::memory_order_relaxed);
            debug_output("the accepted startup log records could not be flushed", ERROR_GEN_FAILURE);
        }
    }

    std::mutex start_mutex_;
    bool started_{};
    SessionFacts session_{};
    std::atomic<FC_LogLevel> level_{
#if defined(NDEBUG)
        FC_LOG_ERROR
#else
        FC_LOG_DEBUG
#endif
    };
    std::filesystem::path log_path_;
    std::unique_ptr<LogWriter> writer_;
    std::unique_ptr<StatusPublisher> status_;
    std::shared_ptr<quill::Sink> sink_;
    std::atomic<BackendLogger*> logger_{};
    const catalog::Catalog* catalog_{};
    std::vector<LiveStatusSection> latest_live_;
    std::atomic_uint64_t accepted_{};
    std::atomic_uint64_t written_{};
    std::atomic_uint64_t dropped_{};
    std::atomic_bool output_failed_{};
    std::atomic_size_t queued_bytes_{};
    std::atomic_uint64_t flush_generation_{};
    std::chrono::steady_clock::time_point last_drop_report_{};
    std::uint64_t reported_drops_{};
};

ReportingSession::ReportingSession() : state_(new (std::nothrow) State) {}

ReportingSession::~ReportingSession() = default;

void ReportingSession::start(SessionFacts facts, std::filesystem::path output_directory) noexcept {
    if (state_) {
        state_->start(std::move(facts), std::move(output_directory));
    }
}

void ReportingSession::configure(FC_LogLevel level) noexcept {
    if (state_) {
        state_->configure(level);
    }
}

void ReportingSession::set_target(const targets::RecognizedTarget& target) noexcept {
    try {
        if (state_ && state_->status_) {
            state_->status_->set_target(target);
        }
    } catch (...) {
        debug_output("the recognized target could not be copied into status", ERROR_GEN_FAILURE);
    }
}

void ReportingSession::set_catalog(const catalog::Catalog& catalog,
                                   std::span<const catalog::RejectionRecord> rejections) noexcept {
    try {
        if (!state_) {
            return;
        }
        state_->catalog_ = &catalog;
        if (state_->status_) {
            state_->status_->set_catalog(catalog, rejections, std::filesystem::path{"config"} / "FC.Core.ini");
        }
    } catch (...) {
        debug_output("the admitted plugin catalog could not be copied into reporting", ERROR_GEN_FAILURE);
    }
}

CoreLogger ReportingSession::logger(std::string_view scope) noexcept {
    if (!state_) {
        return {};
    }
    return {state_.get(), scope,
            [](const void* context, FC_LogLevel level) noexcept {
                return static_cast<const State*>(context)->enabled(level);
            },
            [](void* context, std::string_view component, FC_LogLevel level, std::string_view message) noexcept {
                auto& state = *static_cast<State*>(context);
                try {
                    std::string source{"FusionCutter/"};
                    source.append(component);
                    // The common producer path gives framework and plugin records identical filtering and capacity.
                    if (message.size() <= std::numeric_limits<std::uint32_t>::max()) {
                        const FC_StringView view{message.data(), static_cast<std::uint32_t>(message.size())};
                        state.enqueue(source, level, view);
                    } else {
                        state.dropped_.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    state.dropped_.fetch_add(1, std::memory_order_relaxed);
                    debug_output("a framework log record could not be scoped", ERROR_GEN_FAILURE);
                }
            }};
}

FC_Bool ReportingSession::enabled(FC_ReportToken, FC_LogLevel level) const noexcept {
    return state_ && state_->enabled(level) ? FC_TRUE : FC_FALSE;
}

void ReportingSession::write(FC_ReportToken report, FC_LogLevel level, FC_StringView message) noexcept {
    // Resolve and allocate source attribution only after the configured filter accepts this dynamic message.
    if (!state_ || !state_->enabled(level)) {
        return;
    }
    try {
        // Token resolution stays inside reporting so plugins cannot forge or format their own source identities.
        state_->enqueue(state_->source(report), level, message);
    } catch (...) {
        state_->dropped_.fetch_add(1, std::memory_order_relaxed);
        debug_output("a plugin log source could not be resolved", ERROR_GEN_FAILURE);
    }
}

void ReportingSession::publish(InitializationStatus initialization, const planning::FailureReason* reason,
                               const runtime::PatchRuntimeState* runtime, const runtime::TraceSession& traces,
                               bool force) noexcept {
    try {
        if (!state_ || !state_->status_) {
            return;
        }
        // Gate the whole opportunity before plugin code so status callbacks share the renderer's one-second cadence.
        if (!state_->status_->publication_due(force)) {
            return;
        }
        // Contributors run only on ordinary publication; their last safe values are retained for fatal paths.
        if (runtime != nullptr) {
            auto collected = state_->status_->collect_live(*runtime);
            for (const auto& section : collected) {
                if (!section.callback_failed) {
                    continue;
                }
                const auto previous = std::ranges::find_if(state_->latest_live_, [&](const auto& candidate) {
                    return candidate.patch == section.patch;
                });
                if (previous == state_->latest_live_.end() || !previous->callback_failed) {
                    logger("Status").warning(
                        "Patch '{}' live status callback failed; its remaining fields were omitted",
                        runtime->catalog.patch(section.patch).id);
                }
            }
            state_->latest_live_ = std::move(collected);
        }
        const auto result = state_->status_->publish(initialization, reason, runtime, state_->latest_live_,
                                                     state_->log_status(), traces.status(), force);
        if (result == StatusPublishResult::Failed) {
            constexpr std::string_view message =
                "FusionCutter.txt could not be published; a later status opportunity will retry";
            write(nullptr, FC_LOG_ERROR, {message.data(), static_cast<std::uint32_t>(message.size())});
        }
        // Publication of a fatal result is synchronous so accepted diagnostics are attempted before termination.
        if (initialization == InitializationStatus::Fatal) {
            state_->flush();
        }
    } catch (...) {
        debug_output("status publication failed unexpectedly", ERROR_GEN_FAILURE);
    }
}

void ReportingSession::fatal(std::string_view reason, const runtime::PatchRuntimeState* runtime,
                             const runtime::TraceSession& traces) noexcept {
    if (!state_) {
        return;
    }
    // Invariants after the Commit phase bypass ordinary initialization results, so publish their cause before flushing.
    logger("Runtime").error("Fatal runtime invariant: {}", reason);
    const planning::FailureReason failure{.message = std::string{reason},
                                          .phase = planning::PatchPhase::Activate,
                                          .operation = "Enforce invariant after the Commit phase"};
    // Reuse latest_live_ deliberately: a fatal path must not call any status contributor.
    if (state_->status_) {
        const auto result = state_->status_->publish(InitializationStatus::Fatal, &failure, runtime,
                                                     state_->latest_live_, state_->log_status(), traces.status(), true);
        if (result == StatusPublishResult::Failed) {
            constexpr std::string_view message = "FusionCutter.txt could not be published before fatal termination";
            write(nullptr, FC_LOG_ERROR, {message.data(), static_cast<std::uint32_t>(message.size())});
        }
    }
    state_->flush();
}

} // namespace fc::reporting
