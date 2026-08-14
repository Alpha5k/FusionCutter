#include <FusionCutter/diagnostics.hpp>

#include "diagnostics.hpp"
#include "spsc_ring.hpp"

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
#include <intrin.h>
#include <limits>
#include <mutex>
#include <new>
#include <ranges>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fusioncutter::diagnostics_detail {
namespace {

TRACELOGGING_DEFINE_PROVIDER(gProvider, "FusionCutter.Diagnostics",
                             (0x2119725d, 0xf8cf, 0x4855, 0x9c, 0x95, 0xc3, 0x7d, 0xba, 0xb6, 0xdc, 0x65));

constexpr std::size_t kChannelCount = 8;
constexpr std::size_t kProducerCount = 8;
constexpr std::size_t kRingCapacity = 4096;
constexpr std::size_t kBatchCapacity = 64;
constexpr auto kPayloadCapacity = diagnostics::kMaximumEtlPayloadSize;
constexpr auto kDrainInterval = std::chrono::milliseconds{2};
constexpr auto kHealthInterval = std::chrono::seconds{1};
constexpr auto kClockInterval = std::chrono::seconds{5};
constexpr std::uint32_t kDefaultMaximumFileSizeMb = 512;
constexpr std::uint32_t kTransportSchemaVersion = 1;
constexpr std::uint32_t kEtwBufferSizeKb = 64;
constexpr std::uint32_t kEtwMinimumBuffers = 32;
constexpr std::uint32_t kEtwMaximumBuffers = 128;

struct Stamp {
    std::uint64_t timestamp;
    std::uint16_t processor;
};

[[nodiscard]] Stamp read_stamp() noexcept {
    _mm_lfence();
    unsigned int processor{};
    const auto value = __rdtscp(&processor);
    _mm_lfence();
    return {value, static_cast<std::uint16_t>(processor)};
}

// Carries one bounded observation from a game callback to the shared ETL writer.
struct Record {
    std::uint64_t timestamp;
    std::uint64_t sequence;
    std::uint32_t thread_id;
    std::uint32_t context;
    std::uint16_t kind;
    std::uint16_t flags;
    std::uint16_t processor;
    std::uint8_t channel;
    std::uint8_t payload_size;
    std::array<std::byte, kPayloadCapacity> payload;
};

static_assert(sizeof(Record) == 128);

struct Producer {
    std::atomic<DWORD> owner{};
    std::atomic<std::uint64_t> sequence{};
    std::atomic<std::uint32_t> high_water{};
    std::uint16_t last_processor{};
    bool has_processor{};
    SpscRing<Record, kRingCapacity> records;
};

struct ChannelState {
    std::string name;
    std::uint32_t schema_version{};
    std::uint8_t capture_mode{};
    std::atomic<bool> active{};
    std::atomic<std::uint64_t> submitted{};
    std::atomic<std::uint64_t> emitted_records{};
    std::atomic<std::uint64_t> dropped{};
    std::atomic<std::uint64_t> omitted{};
};

struct ThreadProducer {
    const void* session{};
    Producer* producer{};
};

thread_local ThreadProducer gThreadProducer;
std::atomic<std::uint32_t> gMaximumFileSizeMb{kDefaultMaximumFileSizeMb};

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
    return std::format(L"FusionCutter.Diagnostics.{}.{}", GetCurrentProcessId(),
                       reinterpret_cast<std::uintptr_t>(&gProvider));
}

class Session {
  public:
    static Session& instance() noexcept {
        // The service outlives patch globals so their destructors can close channels safely during DLL teardown.
        static auto* session = new Session;
        return *session;
    }

    [[nodiscard]] std::expected<std::uint8_t, OutcomeReason> open_channel(const TargetContext& target,
                                                                          std::string_view name,
                                                                          std::uint32_t schema_version,
                                                                          std::uint8_t capture_mode) {
        const std::scoped_lock lock(mutex_);
        if (name.empty() || name.size() > 63 || schema_version == 0) {
            return std::unexpected(
                OutcomeReason{"diagnostics channel metadata is invalid", "Prepare diagnostics trace", {}});
        }
        for (std::uint8_t index{}; index < channel_count_; ++index) {
            if (channels_[index].name == name) {
                return std::unexpected(
                    OutcomeReason{"diagnostics channel '" + std::string(name) + "' was prepared more than once",
                                  "Prepare diagnostics trace",
                                  {}});
            }
        }
        if (channel_count_ == channels_.size()) {
            return std::unexpected(
                OutcomeReason{"the diagnostics channel capacity is exhausted", "Prepare diagnostics trace", {}});
        }
        if (!prepared_) {
            if (auto prepared = prepare_session(target); !prepared.has_value()) {
                return std::unexpected(std::move(prepared.error()));
            }
        } else if (target.role != role_ || target.layout != layout_ || target.image.architecture != architecture_) {
            return std::unexpected(OutcomeReason{
                "diagnostics channels do not describe the same target process", "Prepare diagnostics trace", {}});
        }

        auto& channel = channels_[channel_count_];
        channel.name = name;
        channel.schema_version = schema_version;
        channel.capture_mode = capture_mode;
        const auto id = static_cast<std::uint8_t>(channel_count_ + 1);
        ++channel_count_;
        published_channel_count_.store(channel_count_, std::memory_order_release);
        ++open_channels_;
        return id;
    }

    void close_channel(std::uint8_t id) noexcept {
        stop_channel(id);
        const std::scoped_lock lock(mutex_);
        if (id == 0 || id > channel_count_ || open_channels_ == 0) {
            return;
        }
        --open_channels_;
        if (open_channels_ == 0) {
            shutdown_locked();
        }
    }

    void start_channel(std::uint8_t id) noexcept {
        auto* channel = channel_for(id);
        if (channel == nullptr || channel->active.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        active_channels_.fetch_add(1, std::memory_order_acq_rel);
        accepting_.store(true, std::memory_order_release);
    }

    void stop_channel(std::uint8_t id) noexcept {
        auto* channel = channel_for(id);
        if (channel == nullptr || !channel->active.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (active_channels_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            accepting_.store(false, std::memory_order_release);
            stop_writer();
        }
    }

    void submit(std::uint8_t channel_id, std::uint16_t kind, std::span<const std::byte> payload, std::uint32_t context,
                std::uint16_t flags) noexcept {
        auto* channel = channel_for(channel_id);
        if (channel == nullptr || !channel->active.load(std::memory_order_acquire) ||
            !accepting_.load(std::memory_order_acquire)) {
            return;
        }
        if (payload.size() > diagnostics::kMaximumEtlPayloadSize) {
            channel->dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto* producer = producer_for_current_thread();
        if (producer == nullptr) {
            unexpected_thread_records_.fetch_add(1, std::memory_order_relaxed);
            channel->dropped.fetch_add(1, std::memory_order_relaxed);
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
            .sequence = producer->sequence.fetch_add(1, std::memory_order_relaxed),
            .thread_id = GetCurrentThreadId(),
            .context = context,
            .kind = kind,
            .flags = flags,
            .processor = stamp.processor,
            .channel = channel_id,
            .payload_size = static_cast<std::uint8_t>(payload.size()),
        };
        std::ranges::copy(payload.first(record.payload_size), record.payload.begin());

        if (!producer->records.push(record)) {
            channel->dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        channel->submitted.fetch_add(1, std::memory_order_relaxed);
        const auto depth = producer->records.size();
        auto high_water = producer->high_water.load(std::memory_order_relaxed);
        while (depth > high_water &&
               !producer->high_water.compare_exchange_weak(high_water, depth, std::memory_order_relaxed)) {}
    }

    void omit(std::uint8_t id, std::uint64_t count) noexcept {
        if (auto* channel = channel_for(id); channel != nullptr) {
            channel->omitted.fetch_add(count, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] diagnostics::TraceHealth health(std::uint8_t id) const noexcept {
        const auto* channel = channel_for(id);
        if (channel == nullptr) {
            return {};
        }

        std::uint32_t high_water{};
        for (const auto& producer : producers_) {
            high_water = (std::max)(high_water, producer.high_water.load(std::memory_order_relaxed));
        }
        return {
            .submitted = channel->submitted.load(std::memory_order_relaxed),
            .emitted_records = channel->emitted_records.load(std::memory_order_relaxed),
            .dropped = channel->dropped.load(std::memory_order_relaxed),
            .omitted = channel->omitted.load(std::memory_order_relaxed),
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
    [[nodiscard]] ChannelState* channel_for(std::uint8_t id) noexcept {
        const auto count = published_channel_count_.load(std::memory_order_acquire);
        return id != 0 && id <= count ? &channels_[id - 1] : nullptr;
    }

    [[nodiscard]] const ChannelState* channel_for(std::uint8_t id) const noexcept {
        const auto count = published_channel_count_.load(std::memory_order_acquire);
        return id != 0 && id <= count ? &channels_[id - 1] : nullptr;
    }

    [[nodiscard]] EVENT_TRACE_PROPERTIES* trace_properties() noexcept {
        return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties_.data());
    }

    [[nodiscard]] std::expected<void, OutcomeReason> prepare_session(const TargetContext& target) {
        const auto directory = core_directory();
        if (directory.empty()) {
            return std::unexpected(OutcomeReason{
                "could not locate FusionCutter.dll for diagnostics trace output", "Prepare diagnostics trace", {}});
        }

        path_ = directory / trace_basename(target.role);
        filename_ = path_.filename().string();
        session_name_ = session_name();
        role_ = target.role;
        layout_ = target.layout;
        architecture_ = target.image.architecture;
        maximum_file_size_ =
            static_cast<std::uint64_t>(gMaximumFileSizeMb.load(std::memory_order_relaxed)) * 1024 * 1024;

        const auto registration = TraceLoggingRegister(gProvider);
        if (registration != ERROR_SUCCESS) {
            return std::unexpected(OutcomeReason{
                "could not register the diagnostics TraceLogging provider", "Prepare diagnostics trace", {}});
        }
        provider_registered_ = true;

        const auto property_size =
            sizeof(EVENT_TRACE_PROPERTIES) + (session_name_.size() + path_.native().size() + 2) * sizeof(wchar_t);
        properties_.resize(property_size);
        auto* properties = trace_properties();
        properties->Wnode.BufferSize = static_cast<ULONG>(property_size);
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->Wnode.ClientContext = 1;
        properties->BufferSize = kEtwBufferSizeKb;
        properties->MinimumBuffers = kEtwMinimumBuffers;
        properties->MaximumBuffers = kEtwMaximumBuffers;
        properties->FlushTimer = 1;
        properties->LogFileMode =
            EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_PRIVATE_LOGGER_MODE | EVENT_TRACE_PRIVATE_IN_PROC;
        properties->MaximumFileSize = gMaximumFileSizeMb.load(std::memory_order_relaxed);
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
                OutcomeReason{"could not create the diagnostics ETL session", "Prepare diagnostics trace", {}});
        }

        const auto provider_id = TraceLoggingProviderId(gProvider);
        const auto enabled = EnableTraceEx2(session_, &provider_id, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                            TRACE_LEVEL_VERBOSE, std::numeric_limits<ULONGLONG>::max(), 0, 0, nullptr);
        if (enabled != ERROR_SUCCESS) {
            stop_session();
            cleanup_provider();
            return std::unexpected(
                OutcomeReason{"could not enable the diagnostics ETL provider", "Prepare diagnostics trace", {}});
        }

        try {
            writer_ = std::jthread([this](std::stop_token stop) {
                run(stop);
            });
        } catch (const std::system_error&) {
            stop_session();
            cleanup_provider();
            return std::unexpected(
                OutcomeReason{"could not start the diagnostics trace writer", "Prepare diagnostics trace", {}});
        }

        prepared_ = true;
        return {};
    }

    [[nodiscard]] Producer* producer_for_current_thread() noexcept {
        if (gThreadProducer.session == this) {
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

        TraceLoggingWrite(gProvider, "SessionStart", TraceLoggingString("FusionCutter.Diagnostics", "Schema"),
                          TraceLoggingUInt32(kTransportSchemaVersion, "SchemaVersion"),
                          TraceLoggingUInt32(GetCurrentProcessId(), "ProcessId"),
                          TraceLoggingUInt8(static_cast<std::uint8_t>(role_), "Role"),
                          TraceLoggingUInt8(static_cast<std::uint8_t>(layout_), "TargetLayout"),
                          TraceLoggingUInt8(static_cast<std::uint8_t>(architecture_), "Architecture"),
                          TraceLoggingUInt64(static_cast<std::uint64_t>(qpc.QuadPart), "Qpc"),
                          TraceLoggingUInt64(static_cast<std::uint64_t>(frequency.QuadPart), "QpcFrequency"),
                          TraceLoggingUInt64(utc_value, "UtcFileTime"),
                          TraceLoggingUInt64(read_stamp().timestamp, "Tsc"));
    }

    void write_channel_definitions() noexcept {
        const auto count = published_channel_count_.load(std::memory_order_acquire);
        while (written_channel_count_ < count) {
            const auto id = static_cast<std::uint8_t>(written_channel_count_ + 1);
            const auto& channel = channels_[written_channel_count_++];
            TraceLoggingWrite(gProvider, "ChannelDefinition", TraceLoggingUInt8(id, "Channel"),
                              TraceLoggingString(channel.name.c_str(), "Name"),
                              TraceLoggingUInt32(channel.schema_version, "SchemaVersion"),
                              TraceLoggingUInt8(channel.capture_mode, "CaptureMode"));
        }
    }

    void write_clock() noexcept {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        TraceLoggingWrite(gProvider, "Clock", TraceLoggingUInt64(static_cast<std::uint64_t>(qpc.QuadPart), "Qpc"),
                          TraceLoggingUInt64(read_stamp().timestamp, "Tsc"));
    }

    void write_health() noexcept {
        refresh_session_health();
        const auto channel_count = published_channel_count_.load(std::memory_order_acquire);
        TraceLoggingWrite(
            gProvider, "SessionHealth",
            TraceLoggingUInt64(writer_errors_.load(std::memory_order_relaxed), "WriterErrors"),
            TraceLoggingUInt64(etw_events_lost_.load(std::memory_order_relaxed), "EtwEventsLost"),
            TraceLoggingUInt64(etw_buffers_lost_.load(std::memory_order_relaxed), "EtwBuffersLost"),
            TraceLoggingUInt32(unexpected_thread_records_.load(std::memory_order_relaxed), "UnexpectedThreadRecords"),
            TraceLoggingUInt32(core_migrations_.load(std::memory_order_relaxed), "CoreMigrations"),
            TraceLoggingBool(file_limit_reached_.load(std::memory_order_relaxed), "FileLimitReached"));
        for (std::uint8_t index{}; index < channel_count; ++index) {
            const auto snapshot = health(static_cast<std::uint8_t>(index + 1));
            TraceLoggingWrite(gProvider, "ChannelHealth", TraceLoggingUInt8(index + 1, "Channel"),
                              TraceLoggingUInt64(snapshot.submitted, "Submitted"),
                              TraceLoggingUInt64(snapshot.emitted_records, "EmittedRecords"),
                              TraceLoggingUInt64(snapshot.dropped, "Dropped"),
                              TraceLoggingUInt64(snapshot.omitted, "Omitted"),
                              TraceLoggingUInt32(snapshot.high_water, "RingHighWater"));
        }
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

    [[nodiscard]] bool write_batch(std::span<const Record> records, std::uint32_t producer) noexcept {
        if (TraceLoggingProviderEnabled(gProvider, TRACE_LEVEL_VERBOSE, 0) == FALSE) {
            writer_errors_.fetch_add(1, std::memory_order_relaxed);
            accepting_.store(false, std::memory_order_release);
            return false;
        }

        TraceLoggingWrite(gProvider, "RecordBatch", TraceLoggingUInt32(producer, "Producer"),
                          TraceLoggingUInt16(static_cast<std::uint16_t>(records.size()), "Count"),
                          TraceLoggingBinary(records.data(), static_cast<ULONG>(records.size_bytes()), "Records"));
        for (const auto& record : records) {
            if (auto* channel = channel_for(record.channel); channel != nullptr) {
                channel->emitted_records.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return true;
    }

    [[nodiscard]] bool drain() noexcept {
        bool wrote{};
        std::array<Record, kBatchCapacity> batch{};
        for (std::uint32_t producer{}; producer < producers_.size(); ++producer) {
            for (;;) {
                std::size_t count{};
                while (count < batch.size() && producers_[producer].records.pop(batch[count])) {
                    ++count;
                }
                if (count == 0) {
                    break;
                }
                wrote = true;
                if (!write_batch(std::span{batch}.first(count), producer)) {
                    return wrote;
                }
            }
        }
        return wrote;
    }

    void run(std::stop_token stop) noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (!stop.stop_requested() && !accepting_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kDrainInterval);
        }
        if (stop.stop_requested()) {
            return;
        }

        write_session();
        write_channel_definitions();
        auto next_health = std::chrono::steady_clock::now() + kHealthInterval;
        auto next_clock = std::chrono::steady_clock::now() + kClockInterval;

        while (!stop.stop_requested() && accepting_.load(std::memory_order_acquire)) {
            write_channel_definitions();
            const auto wrote = drain();
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_health) {
                write_health();
                std::error_code error;
                const auto file_size = std::filesystem::file_size(path_, error);
                if (!error && file_size >= maximum_file_size_) {
                    file_limit_reached_.store(true, std::memory_order_relaxed);
                    accepting_.store(false, std::memory_order_release);
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
        write_channel_definitions();
        write_health();
        write_session_end();
    }

    void stop_writer() noexcept {
        if (writer_.joinable()) {
            writer_.request_stop();
            writer_.join();
        }
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

    void shutdown_locked() noexcept {
        accepting_.store(false, std::memory_order_release);
        stop_writer();
        stop_session();
        cleanup_provider();
        prepared_ = false;
    }

    mutable std::mutex mutex_;
    std::array<ChannelState, kChannelCount> channels_{};
    std::array<Producer, kProducerCount> producers_{};
    std::atomic<std::uint8_t> published_channel_count_{};
    std::atomic<std::uint8_t> active_channels_{};
    std::atomic<bool> accepting_{};
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
    Architecture architecture_{};
    std::size_t open_channels_{};
    std::uint8_t channel_count_{};
    std::uint8_t written_channel_count_{};
    bool prepared_{};
    bool provider_registered_{};
    bool health_query_failed_{};
};

} // namespace

void configure(std::uint32_t maximum_file_size_mb) noexcept {
    gMaximumFileSizeMb.store(maximum_file_size_mb, std::memory_order_relaxed);
}

} // namespace fusioncutter::diagnostics_detail

namespace fusioncutter::diagnostics {

EtlChannel::EtlChannel() noexcept = default;

EtlChannel::~EtlChannel() {
    if (channel_ != 0) {
        diagnostics_detail::Session::instance().close_channel(channel_);
    }
}

EtlChannel::EtlChannel(EtlChannel&& other) noexcept : channel_(std::exchange(other.channel_, std::uint8_t{})) {}

EtlChannel& EtlChannel::operator=(EtlChannel&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (channel_ != 0) {
        diagnostics_detail::Session::instance().close_channel(channel_);
    }
    channel_ = std::exchange(other.channel_, std::uint8_t{});
    return *this;
}

std::expected<void, OutcomeReason> EtlChannel::prepare(const TargetContext& target, std::string_view name,
                                                       std::uint32_t schema_version, std::uint8_t capture_mode) {
    if (channel_ != 0) {
        return {};
    }
    auto channel = diagnostics_detail::Session::instance().open_channel(target, name, schema_version, capture_mode);
    if (!channel.has_value()) {
        return std::unexpected(std::move(channel.error()));
    }
    channel_ = *channel;
    return {};
}

void EtlChannel::start() noexcept {
    diagnostics_detail::Session::instance().start_channel(channel_);
}

void EtlChannel::stop() noexcept {
    diagnostics_detail::Session::instance().stop_channel(channel_);
}

void EtlChannel::submit(std::uint16_t kind, std::span<const std::byte> payload, std::uint32_t context,
                        std::uint16_t flags) noexcept {
    diagnostics_detail::Session::instance().submit(channel_, kind, payload, context, flags);
}

void EtlChannel::omit(std::uint64_t count) noexcept {
    diagnostics_detail::Session::instance().omit(channel_, count);
}

TraceHealth EtlChannel::health() const noexcept {
    return diagnostics_detail::Session::instance().health(channel_);
}

std::string_view EtlChannel::filename() const noexcept {
    return diagnostics_detail::Session::instance().filename();
}

} // namespace fusioncutter::diagnostics
