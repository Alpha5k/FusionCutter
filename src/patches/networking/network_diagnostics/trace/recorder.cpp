#include "recorder.hpp"

#include "ring.hpp"

#include <Windows.h>
#include <TraceLoggingProvider.h>
#include <evntrace.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <new>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fusioncutter::patches::network_diagnostics::trace {
namespace {

TRACELOGGING_DEFINE_PROVIDER(gProvider, "FusionCutter.NetworkDiagnostics",
                             (0x54861866, 0x6219, 0x42d4, 0xaa, 0xb6, 0x13, 0x27, 0x60, 0xc0, 0x93, 0x37));

constexpr std::size_t kProducerCount = 4;
constexpr std::size_t kRingCapacity = 4096;
constexpr auto kDrainInterval = std::chrono::milliseconds{2};
constexpr auto kHealthInterval = std::chrono::seconds{1};
constexpr auto kClockInterval = std::chrono::seconds{5};

struct Producer {
    std::atomic<DWORD> owner{};
    std::atomic<std::uint64_t> submitted{};
    std::atomic<std::uint64_t> dropped{};
    std::atomic<std::uint32_t> high_water{};
    std::uint16_t last_processor{};
    bool has_processor{};
    SpscRing<Record, kRingCapacity> records;
};

struct ThreadProducer {
    const void* recorder{};
    Producer* producer{};
};

thread_local ThreadProducer gThreadProducer;

[[nodiscard]] std::filesystem::path core_directory() {
    HMODULE module{};
    const auto address = reinterpret_cast<const void*>(&core_directory);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) == FALSE) {
        return {};
    }

    std::wstring path(512, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length + 1 < path.size()) {
            path.resize(length);
            return std::filesystem::path{path}.parent_path();
        }
        path.resize(path.size() * 2);
    }
}

[[nodiscard]] std::wstring trace_basename(HostRole role) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const auto role_name = role == HostRole::Server ? L"Server" : L"Client";
    return std::format(L"FusionCutter-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.etl", role_name, time.wYear, time.wMonth,
                       time.wDay, time.wHour, time.wMinute, time.wSecond);
}

[[nodiscard]] std::wstring session_name() {
    return std::format(L"FusionCutter.NetworkDiagnostics.{}.{}", GetCurrentProcessId(),
                       reinterpret_cast<std::uintptr_t>(&gProvider));
}

} // namespace

class Recorder::State {
  public:
    ~State() {
        stop();
    }

    [[nodiscard]] std::expected<void, OutcomeReason> prepare(const TargetContext& target, std::uint8_t capture_mode,
                                                             std::uint32_t maximum_file_size_mb) {
        if (prepared_) {
            return {};
        }

        const auto directory = core_directory();
        if (directory.empty()) {
            return std::unexpected(OutcomeReason{
                "could not locate FusionCutter.dll for network trace output", "Prepare network trace", {}});
        }

        const auto basename = trace_basename(target.role);
        path_ = directory / basename;
        filename_ = path_.filename().string();
        session_name_ = session_name();
        role_ = target.role;
        layout_ = target.layout;
        capture_mode_ = capture_mode;

        const auto registration = TraceLoggingRegister(gProvider);
        if (registration != ERROR_SUCCESS) {
            return std::unexpected(OutcomeReason{
                "could not register the network diagnostics TraceLogging provider", "Prepare network trace", {}});
        }
        provider_registered_ = true;
        maximum_file_size_ = static_cast<std::uint64_t>(maximum_file_size_mb) * 1024 * 1024;

        const auto property_size =
            sizeof(EVENT_TRACE_PROPERTIES) + (session_name_.size() + path_.native().size() + 2) * sizeof(wchar_t);
        properties_.resize(property_size);
        auto* properties = trace_properties();
        properties->Wnode.BufferSize = static_cast<ULONG>(property_size);
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->Wnode.ClientContext = 1;
        properties->LogFileMode =
            EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_PRIVATE_LOGGER_MODE | EVENT_TRACE_PRIVATE_IN_PROC;
        properties->MaximumFileSize = maximum_file_size_mb;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        properties->LogFileNameOffset =
            properties->LoggerNameOffset + static_cast<ULONG>((session_name_.size() + 1) * sizeof(wchar_t));

        auto* logger_name = reinterpret_cast<wchar_t*>(properties_.data() + properties->LoggerNameOffset);
        std::memcpy(logger_name, session_name_.c_str(), (session_name_.size() + 1) * sizeof(wchar_t));
        auto* log_name = reinterpret_cast<wchar_t*>(properties_.data() + properties->LogFileNameOffset);
        std::memcpy(log_name, path_.c_str(), (path_.native().size() + 1) * sizeof(wchar_t));

        const auto started = StartTraceW(&session_, session_name_.c_str(), properties);
        if (started != ERROR_SUCCESS) {
            cleanup_provider();
            return std::unexpected(
                OutcomeReason{"could not create the network diagnostics ETL session", "Prepare network trace", {}});
        }

        const auto provider_id = TraceLoggingProviderId(gProvider);
        const auto enabled = EnableTraceEx2(session_, &provider_id, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                            TRACE_LEVEL_VERBOSE, std::numeric_limits<ULONGLONG>::max(), 0, 0, nullptr);
        if (enabled != ERROR_SUCCESS) {
            stop_session();
            cleanup_provider();
            return std::unexpected(
                OutcomeReason{"could not enable the network diagnostics ETL provider", "Prepare network trace", {}});
        }

        try {
            writer_ = std::jthread([this](std::stop_token stop) {
                run(stop);
            });
        } catch (const std::system_error&) {
            stop_session();
            cleanup_provider();
            return std::unexpected(
                OutcomeReason{"could not start the network trace writer", "Prepare network trace", {}});
        }

        prepared_ = true;
        return {};
    }

    void start() noexcept {
        if (!prepared_ || active_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
    }

    void stop() noexcept {
        active_.store(false, std::memory_order_release);
        if (writer_.joinable()) {
            writer_.request_stop();
            writer_.join();
        }
        if (prepared_) {
            stop_session();
            cleanup_provider();
            prepared_ = false;
        }
    }

    void submit(RecordKind kind, std::span<const std::byte> payload, std::uint32_t context, std::uint16_t flags,
                Carrier carrier) noexcept {
        if (!active_.load(std::memory_order_acquire)) {
            return;
        }

        auto* producer = producer_for_current_thread();
        if (producer == nullptr) {
            unexpected_thread_records_.fetch_add(1, std::memory_order_relaxed);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const auto stamp = read_stamp();
        if (producer->has_processor && producer->last_processor != stamp.processor) {
            core_migrations_.fetch_add(1, std::memory_order_relaxed);
        }
        producer->last_processor = stamp.processor;
        producer->has_processor = true;

        Record record{
            .timestamp = stamp.timestamp,
            .sequence = producer->submitted.fetch_add(1, std::memory_order_relaxed),
            .thread_id = GetCurrentThreadId(),
            .context = context,
            .kind = kind,
            .flags = flags,
            .processor = stamp.processor,
            .carrier = carrier,
            .payload_size = static_cast<std::uint8_t>((std::min)(payload.size(), kPayloadCapacity)),
        };
        std::ranges::copy(payload.first(record.payload_size), record.payload.begin());

        if (!producer->records.push(record)) {
            producer->dropped.fetch_add(1, std::memory_order_relaxed);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        submitted_.fetch_add(1, std::memory_order_relaxed);
        const auto depth = producer->records.size();
        auto high_water = producer->high_water.load(std::memory_order_relaxed);
        while (depth > high_water &&
               !producer->high_water.compare_exchange_weak(high_water, depth, std::memory_order_relaxed)) {}
    }

    void omit(std::uint64_t count) noexcept {
        omitted_.fetch_add(count, std::memory_order_relaxed);
    }

    [[nodiscard]] Health health() const noexcept {
        std::uint32_t high_water{};
        for (const auto& producer : producers_) {
            high_water = (std::max)(high_water, producer.high_water.load(std::memory_order_relaxed));
        }
        return {
            .submitted = submitted_.load(std::memory_order_relaxed),
            .emitted_records = emitted_records_.load(std::memory_order_relaxed),
            .dropped = dropped_.load(std::memory_order_relaxed),
            .omitted = omitted_.load(std::memory_order_relaxed),
            .writer_errors = writer_errors_.load(std::memory_order_relaxed),
            .etw_events_lost = etw_events_lost_.load(std::memory_order_relaxed),
            .etw_buffers_lost = etw_buffers_lost_.load(std::memory_order_relaxed),
            .high_water = high_water,
            .unexpected_thread_records = unexpected_thread_records_.load(std::memory_order_relaxed),
            .core_migrations = core_migrations_.load(std::memory_order_relaxed),
            .file_limit_reached = file_limit_reached_.load(std::memory_order_relaxed),
        };
    }

    [[nodiscard]] std::string_view filename() const noexcept {
        return filename_;
    }

  private:
    [[nodiscard]] EVENT_TRACE_PROPERTIES* trace_properties() noexcept {
        return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties_.data());
    }

    [[nodiscard]] Producer* producer_for_current_thread() noexcept {
        if (gThreadProducer.recorder == this) {
            return gThreadProducer.producer;
        }

        const auto thread = GetCurrentThreadId();
        for (auto& producer : producers_) {
            auto owner = producer.owner.load(std::memory_order_acquire);
            if (owner == thread ||
                (owner == 0 && producer.owner.compare_exchange_strong(owner, thread, std::memory_order_acq_rel))) {
                gThreadProducer = {this, &producer};
                return &producer;
            }
        }
        return nullptr;
    }

    void write_session() noexcept {
        LARGE_INTEGER qpc{};
        LARGE_INTEGER frequency{};
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&frequency);
        FILETIME utc{};
        GetSystemTimePreciseAsFileTime(&utc);
        const auto utc_value = (static_cast<std::uint64_t>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;

        TraceLoggingWrite(
            gProvider, "SessionStart", TraceLoggingString("FusionCutter.NetworkDiagnostics", "Schema"),
            TraceLoggingUInt32(kSchemaVersion, "SchemaVersion"), TraceLoggingUInt32(GetCurrentProcessId(), "ProcessId"),
            TraceLoggingUInt8(static_cast<std::uint8_t>(role_), "Role"),
            TraceLoggingUInt8(static_cast<std::uint8_t>(layout_), "TargetLayout"),
            TraceLoggingUInt8(capture_mode_, "CaptureMode"),
            TraceLoggingUInt64(static_cast<std::uint64_t>(qpc.QuadPart), "Qpc"),
            TraceLoggingUInt64(static_cast<std::uint64_t>(frequency.QuadPart), "QpcFrequency"),
            TraceLoggingUInt64(utc_value, "UtcFileTime"), TraceLoggingUInt64(read_stamp().timestamp, "Tsc"));
    }

    void write_clock() noexcept {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        TraceLoggingWrite(gProvider, "Clock", TraceLoggingUInt64(static_cast<std::uint64_t>(qpc.QuadPart), "Qpc"),
                          TraceLoggingUInt64(read_stamp().timestamp, "Tsc"));
    }

    void write_health() noexcept {
        refresh_session_health();
        const auto snapshot = health();
        TraceLoggingWrite(gProvider, "Health", TraceLoggingUInt64(snapshot.submitted, "Submitted"),
                          TraceLoggingUInt64(snapshot.emitted_records, "EmittedRecords"),
                          TraceLoggingUInt64(snapshot.dropped, "Dropped"),
                          TraceLoggingUInt64(snapshot.omitted, "Omitted"),
                          TraceLoggingUInt64(snapshot.writer_errors, "WriterErrors"),
                          TraceLoggingUInt64(snapshot.etw_events_lost, "EtwEventsLost"),
                          TraceLoggingUInt64(snapshot.etw_buffers_lost, "EtwBuffersLost"),
                          TraceLoggingUInt32(snapshot.high_water, "RingHighWater"),
                          TraceLoggingUInt32(snapshot.unexpected_thread_records, "UnexpectedThreadRecords"),
                          TraceLoggingUInt32(snapshot.core_migrations, "CoreMigrations"),
                          TraceLoggingBool(snapshot.file_limit_reached, "FileLimitReached"));
    }

    void write_session_end() noexcept {
        TraceLoggingWrite(gProvider, "SessionEnd", TraceLoggingUInt64(read_stamp().timestamp, "Tsc"));
    }

    void refresh_session_health() noexcept {
        if (session_ == 0) {
            return;
        }

        auto* properties = trace_properties();
        if (ControlTraceW(session_, session_name_.c_str(), properties, EVENT_TRACE_CONTROL_QUERY) != ERROR_SUCCESS) {
            if (!health_query_failed_) {
                writer_errors_.fetch_add(1, std::memory_order_relaxed);
                health_query_failed_ = true;
            }
            return;
        }

        etw_events_lost_.store(properties->EventsLost, std::memory_order_relaxed);
        etw_buffers_lost_.store(properties->LogBuffersLost, std::memory_order_relaxed);
    }

    [[nodiscard]] bool write_record(const Record& record, std::uint32_t producer) noexcept {
        if (TraceLoggingProviderEnabled(gProvider, TRACE_LEVEL_VERBOSE, 0) == FALSE) {
            writer_errors_.fetch_add(1, std::memory_order_relaxed);
            active_.store(false, std::memory_order_release);
            return false;
        }

        TraceLoggingWrite(
            gProvider, "Record", TraceLoggingUInt16(static_cast<std::uint16_t>(record.kind), "Kind"),
            TraceLoggingString(record_kind_name(record.kind), "KindName"), TraceLoggingUInt16(record.flags, "Flags"),
            TraceLoggingUInt8(static_cast<std::uint8_t>(record.carrier), "Carrier"),
            TraceLoggingUInt32(producer, "Producer"), TraceLoggingUInt64(record.sequence, "ProducerSequence"),
            TraceLoggingUInt64(record.timestamp, "Tsc"), TraceLoggingUInt32(record.thread_id, "ThreadId"),
            TraceLoggingUInt16(record.processor, "Processor"), TraceLoggingUInt32(record.context, "Context"),
            TraceLoggingBinary(record.payload.data(), record.payload_size, "Payload"));
        emitted_records_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool drain() noexcept {
        bool wrote{};
        Record record{};
        for (std::uint32_t producer = 0; producer < producers_.size(); ++producer) {
            while (producers_[producer].records.pop(record)) {
                wrote = true;
                if (!write_record(record, producer)) {
                    return wrote;
                }
            }
        }
        return wrote;
    }

    void run(std::stop_token stop) noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (!stop.stop_requested() && !active_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kDrainInterval);
        }
        if (stop.stop_requested()) {
            return;
        }

        write_session();
        auto next_health = std::chrono::steady_clock::now() + kHealthInterval;
        auto next_clock = std::chrono::steady_clock::now() + kClockInterval;

        while (!stop.stop_requested() && active_.load(std::memory_order_acquire)) {
            const auto wrote = drain();
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_health) {
                write_health();
                std::error_code error;
                const auto file_size = std::filesystem::file_size(path_, error);
                if (!error && file_size >= maximum_file_size_) {
                    file_limit_reached_.store(true, std::memory_order_relaxed);
                    active_.store(false, std::memory_order_release);
                }
                next_health = now + kHealthInterval;
            }
            if (now >= next_clock) {
                write_clock();
                next_clock = now + kClockInterval;
            }
            if (!wrote) {
                std::this_thread::sleep_for(kDrainInterval);
            }
        }

        while (drain()) {}
        write_health();
        write_session_end();
    }

    void stop_session() noexcept {
        if (session_ == 0) {
            return;
        }
        static_cast<void>(ControlTraceW(session_, session_name_.c_str(), trace_properties(), EVENT_TRACE_CONTROL_STOP));
        session_ = 0;
    }

    void cleanup_provider() noexcept {
        if (provider_registered_) {
            TraceLoggingUnregister(gProvider);
            provider_registered_ = false;
        }
    }

    std::array<Producer, kProducerCount> producers_{};
    std::atomic<bool> active_{};
    std::atomic<std::uint64_t> submitted_{};
    std::atomic<std::uint64_t> emitted_records_{};
    std::atomic<std::uint64_t> dropped_{};
    std::atomic<std::uint64_t> omitted_{};
    std::atomic<std::uint64_t> writer_errors_{};
    std::atomic<std::uint64_t> etw_events_lost_{};
    std::atomic<std::uint64_t> etw_buffers_lost_{};
    std::atomic<std::uint32_t> unexpected_thread_records_{};
    std::atomic<std::uint32_t> core_migrations_{};
    std::atomic<bool> file_limit_reached_{};
    std::jthread writer_;
    TRACEHANDLE session_{};
    std::vector<std::byte> properties_;
    std::filesystem::path path_;
    std::wstring session_name_;
    std::string filename_;
    std::uint64_t maximum_file_size_{};
    HostRole role_{};
    TargetLayout layout_{};
    std::uint8_t capture_mode_{};
    bool prepared_{};
    bool provider_registered_{};
    bool health_query_failed_{};
};

Recorder::Recorder() noexcept = default;

Recorder::~Recorder() = default;

std::expected<void, OutcomeReason> Recorder::prepare(const TargetContext& target, std::uint8_t capture_mode,
                                                     std::uint32_t maximum_file_size_mb) {
    if (state_ == nullptr) {
        try {
            state_ = std::make_unique<State>();
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                OutcomeReason{"could not allocate the network trace recorder", "Prepare network trace", {}});
        }
    }
    return state_->prepare(target, capture_mode, maximum_file_size_mb);
}

void Recorder::start() noexcept {
    if (state_ != nullptr) {
        state_->start();
    }
}

void Recorder::stop() noexcept {
    if (state_ != nullptr) {
        state_->stop();
    }
}

void Recorder::submit(RecordKind kind, std::span<const std::byte> payload, std::uint32_t context, std::uint16_t flags,
                      Carrier carrier) noexcept {
    if (state_ != nullptr) {
        state_->submit(kind, payload, context, flags, carrier);
    }
}

void Recorder::omit(std::uint64_t count) noexcept {
    if (state_ != nullptr) {
        state_->omit(count);
    }
}

Health Recorder::health() const noexcept {
    return state_ == nullptr ? Health{} : state_->health();
}

std::string_view Recorder::filename() const noexcept {
    return state_ == nullptr ? std::string_view{} : state_->filename();
}

} // namespace fusioncutter::patches::network_diagnostics::trace
