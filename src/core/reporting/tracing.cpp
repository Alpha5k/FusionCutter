#include "tracing.hpp"

#include "../catalog/definition_copy.hpp"

#include <Windows.h>
#include <evntrace.h>
#include <evntprov.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fc::runtime {

// Producer facts are captured before publication rather than inferred when a later consumer drains the slot.
struct TraceEnvelope {
    std::uint64_t timestamp{};
    std::uint64_t sequence{};
    std::uint32_t thread_id{};
    std::uint32_t payload_size{};
};

// Each sequence value gives one producer exclusive access without a lock and tells the single consumer when ready.
struct TraceSlot {
    std::atomic<std::uint64_t> sequence{};
    std::byte* bytes{};
};

// Queue storage is allocated during the Prepare phase; producer calls perform atomic coordination and one copy.
struct TraceChannelState {
    catalog::PatchIndex patch;
    std::string name;
    std::uint32_t capacity{};
    std::uint32_t maximum_record_size{};
    std::uint32_t version{};
    std::size_t slot_stride{};
    std::size_t storage_bytes{};
    std::unique_ptr<TraceSlot[]> slots;
    std::unique_ptr<std::byte[]> payload_storage;
    std::atomic<std::uint64_t> enqueue_position{};
    std::uint64_t dequeue_position{}; // Read only by the one shared writer thread.
    std::uint32_t channel_id{};
    std::uint32_t handle_generation{};
    bool definition_written{}; // Read only by the writer after the channel becomes armed.
    std::atomic<std::uint64_t> accepted{};
    std::atomic<std::uint64_t> written{};
    std::atomic<std::uint64_t> dropped{};
    std::atomic_bool armed{};
    std::atomic_bool file_limit_reached{};
    std::atomic_bool output_failed{};
};

namespace {

inline constexpr std::size_t kTraceStorageCapacity = 8U * 1024U * 1024U;
inline constexpr std::uint32_t kMaximumRecordSize = 60U * 1024U;

// Handle routing shares the trace storage budget, so malformed tokens can be rejected without a producer-side lock.
constexpr std::size_t kMinimumSlotStride =
    (sizeof(TraceEnvelope) + 1 + alignof(std::uint64_t) - 1) & ~(alignof(std::uint64_t) - 1);
constexpr std::size_t kMinimumChannelStorage = sizeof(TraceChannelState) + 1 + sizeof(TraceSlot) + kMinimumSlotStride;
constexpr std::size_t kHandleEntryStorage = sizeof(std::atomic<TraceChannelState*>) +
                                            sizeof(std::atomic<std::uint32_t>) + sizeof(std::uint32_t) +
                                            sizeof(std::unique_ptr<TraceChannelState>);
constexpr std::size_t kTraceHandleCapacity = kTraceStorageCapacity / (kMinimumChannelStorage + kHandleEntryStorage);
constexpr std::size_t kTraceChannelStorageCapacity = kTraceStorageCapacity - kTraceHandleCapacity * kHandleEntryStorage;
constexpr std::uint32_t kHandleIndexBits = 16;
constexpr std::uint32_t kHandleIndexMask = (1U << kHandleIndexBits) - 1U;
static_assert(kTraceHandleCapacity <= kHandleIndexMask);

// A private provider keeps opaque plugin payloads inside one framework-owned ETL session.
constexpr GUID kTraceProvider = {0xd5645019, 0xaf63, 0x46b9, {0x93, 0xc8, 0x91, 0x0f, 0x22, 0xa7, 0x01, 0x51}};

// Computes the complete reservation charge with overflow checks before any channel storage is allocated.
[[nodiscard]] std::optional<std::size_t> storage_size(std::string_view name, std::uint32_t capacity,
                                                      std::uint32_t maximum_record_size) noexcept {
    const auto payload_size = sizeof(TraceEnvelope) + static_cast<std::size_t>(maximum_record_size);
    constexpr auto alignment = alignof(std::uint64_t);
    if (payload_size > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::nullopt;
    }
    const auto stride = (payload_size + alignment - 1) & ~(alignment - 1);
    if (stride > std::numeric_limits<std::size_t>::max() - sizeof(TraceSlot)) {
        return std::nullopt;
    }
    const auto slot_size = sizeof(TraceSlot) + stride;
    if (capacity > std::numeric_limits<std::size_t>::max() / slot_size) {
        return std::nullopt;
    }
    const auto slots = static_cast<std::size_t>(capacity) * slot_size;
    if (slots > std::numeric_limits<std::size_t>::max() - sizeof(TraceChannelState) - name.size()) {
        return std::nullopt;
    }
    return sizeof(TraceChannelState) + name.size() + slots;
}

// Trace names are human metadata, so control characters and malformed UTF-8 are rejected at creation.
[[nodiscard]] bool valid_name(std::string_view name) noexcept {
    return !name.empty() && catalog::valid_utf8(name) && std::ranges::none_of(name, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte == 0 || byte < 0x20 || byte == 0x7f;
    });
}

// QueryPerformanceCounter gives producers a cheap monotonic timestamp that session metadata calibrates later.
[[nodiscard]] std::uint64_t producer_timestamp() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

// Public health counters remain monotonic even if a process emits more events than their fixed-width representation.
void increment_saturating(std::atomic<std::uint64_t>& value) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint64_t>::max() &&
           !value.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
        // A failed compare-exchange refreshes current for the next saturating attempt.
    }
}

} // namespace

// The writer is prepared during the fallible Prepare phase but creates its directory, session, and file only after an
// armed producer has published its first accepted record.
class TraceSession::Writer final {
  public:
    explicit Writer(TraceSession& owner) noexcept : owner_(&owner) {
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (wake_ == nullptr || stop_ == nullptr) {
            fail_all("Trace writer synchronization events could not be created", GetLastError());
            return;
        }
        try {
            thread_ = std::thread{[this] {
                run();
            }};
        } catch (...) {
            fail_all("Trace writer thread could not be created", ERROR_GEN_FAILURE);
        }
    }

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    ~Writer() {
        if (stop_ != nullptr) {
            SetEvent(stop_);
        }
        if (wake_ != nullptr) {
            SetEvent(wake_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        close_trace();
        if (wake_ != nullptr) {
            CloseHandle(wake_);
        }
        if (stop_ != nullptr) {
            CloseHandle(stop_);
        }
    }

    [[nodiscard]] bool ready() const noexcept {
        return !failed_.load(std::memory_order_relaxed) && thread_.joinable();
    }

    [[nodiscard]] bool failed() const noexcept {
        return failed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool limit_reached() const noexcept {
        return limit_reached_.load(std::memory_order_relaxed);
    }

    void notify() noexcept {
        if (wake_ != nullptr) {
            SetEvent(wake_);
        }
    }

    [[nodiscard]] reporting::TraceStatus status() const noexcept {
        reporting::TraceStatus result;
        result.path = path();
        result.output_failed = failed_.load(std::memory_order_relaxed);
        result.file_limit_reached = limit_reached_.load(std::memory_order_relaxed);
        return result;
    }

  private:
    struct RecordHeader {
        std::uint32_t channel_id{};
        std::uint32_t patch_index{};
        std::uint32_t version{};
        std::uint32_t reserved{};
        TraceEnvelope envelope;
    };

    [[nodiscard]] std::optional<std::filesystem::path> path() const noexcept {
        try {
            const std::scoped_lock lock(path_mutex_);
            return trace_path_.empty() ? std::nullopt : std::optional{trace_path_};
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] bool open_trace() noexcept {
        if (trace_handle_ != 0 && provider_handle_ != 0) {
            return true;
        }
        try {
            // Directory and timestamped identity are deferred until an armed channel produces its first record.
            const auto trace_directory = owner_->installation_directory_ / "traces";
            std::error_code error;
            std::filesystem::create_directories(trace_directory, error);
            if (error) {
                fail_all("Trace output directory could not be created", static_cast<DWORD>(error.value()));
                return false;
            }

            SYSTEMTIME now{};
            GetLocalTime(&now);
            const auto filename =
                std::format(L"FusionCutter-{:04}{:02}{:02}-{:02}{:02}{:02}.{:03}-{}.etl", now.wYear, now.wMonth,
                            now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
            const auto file = trace_directory / filename;
            const auto session_name =
                std::format(L"FusionCutter-{}-{:x}", GetCurrentProcessId(), reinterpret_cast<std::uintptr_t>(this));

            const auto logger_bytes = (session_name.size() + 1) * sizeof(wchar_t);
            const auto file_text = file.native();
            const auto file_bytes = (file_text.size() + 1) * sizeof(wchar_t);
            properties_.assign(sizeof(EVENT_TRACE_PROPERTIES) + logger_bytes + file_bytes, std::byte{});
            auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties_.data());
            // Fixed buffers and sequential mode make ETW enforce the configured file cap as a terminal boundary.
            properties->Wnode.BufferSize = static_cast<ULONG>(properties_.size());
            properties->Wnode.Guid = kTraceProvider;
            properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            properties->Wnode.ClientContext = 1; // QueryPerformanceCounter timestamps.
            properties->BufferSize = 64;
            properties->MinimumBuffers = 2;
            properties->MaximumBuffers = 16;
            properties->MaximumFileSize = owner_->max_trace_size_mb_;
            // An in-process private logger avoids consuming a global ETW session and remains usable by normal users.
            properties->LogFileMode =
                EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_PRIVATE_LOGGER_MODE | EVENT_TRACE_PRIVATE_IN_PROC;
            properties->FlushTimer = 1;
            properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            properties->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + logger_bytes);
            std::memcpy(properties_.data() + properties->LoggerNameOffset, session_name.c_str(), logger_bytes);
            std::memcpy(properties_.data() + properties->LogFileNameOffset, file_text.c_str(), file_bytes);

            // Private sessions bind their control GUID to a provider that this process has already registered.
            auto result = EventRegister(&kTraceProvider, nullptr, nullptr, &provider_handle_);
            if (result != ERROR_SUCCESS) {
                fail_all("Trace ETW provider could not be registered", result);
                return false;
            }
            result = StartTraceW(&trace_handle_, session_name.c_str(), properties);
            if (result != ERROR_SUCCESS) {
                fail_all("Trace ETW session could not be started", result);
                return false;
            }
            result = EnableTraceEx2(trace_handle_, &kTraceProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                    TRACE_LEVEL_VERBOSE, 0, 0, 0, nullptr);
            if (result != ERROR_SUCCESS) {
                fail_all("Trace ETW provider could not be enabled", result);
                return false;
            }

            // Session metadata lets offline consumers interpret QPC timestamps and identify the producing build.
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            const std::wstring version{std::begin(FC_VERSION_STRING), std::end(FC_VERSION_STRING) - 1};
            const std::wstring build{std::begin(FC_BUILD_ID), std::end(FC_BUILD_ID) - 1};
            const auto metadata = std::format(L"Fusion Cutter {} build {}; PID {}; QPC frequency {}", version, build,
                                              GetCurrentProcessId(), frequency.QuadPart);
            result = EventWriteString(provider_handle_, TRACE_LEVEL_INFORMATION, 0, metadata.c_str());
            if (result != ERROR_SUCCESS) {
                fail_all("Trace session metadata could not be written", result);
                return false;
            }
            // Publish the path only after the provider and its identifying metadata are usable.
            {
                const std::scoped_lock lock(path_mutex_);
                trace_path_ = file;
            }
            owner_->logger_.info("Started trace output '{}'", file.string());
            return true;
        } catch (...) {
            fail_all("Trace output could not be initialized", ERROR_GEN_FAILURE);
            return false;
        }
    }

    void write_definition(TraceChannelState& channel) noexcept {
        // The first record for each numeric channel is preceded by stable plugin, patch, and schema identity.
        if (channel.definition_written) {
            return;
        }
        try {
            std::string plugin_id = "Unknown";
            std::string patch_id = std::to_string(channel.patch.value);
            if (owner_->catalog_ != nullptr && channel.patch.value < owner_->catalog_->patch_count()) {
                const auto& plugin = owner_->catalog_->plugin(owner_->catalog_->patch_plugin(channel.patch));
                plugin_id = plugin.definition.id;
                patch_id = owner_->catalog_->patch(channel.patch).id;
            }
            const auto utf8 = std::format("Channel {}: {}/{} name={} version={} capacity={} max_record={}",
                                          channel.channel_id, plugin_id, patch_id, channel.name, channel.version,
                                          channel.capacity, channel.maximum_record_size);
            const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                                      static_cast<int>(utf8.size()), nullptr, 0);
            if (required <= 0) {
                fail_all("Trace channel metadata could not be converted to UTF-16", GetLastError());
                return;
            }
            std::wstring wide(static_cast<std::size_t>(required), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                                required);
            const auto result = EventWriteString(provider_handle_, TRACE_LEVEL_INFORMATION, 0, wide.c_str());
            if (result != ERROR_SUCCESS) {
                fail_all("Trace channel metadata could not be written", result);
                return;
            }
            channel.definition_written = true;
        } catch (...) {
            fail_all("Trace channel metadata could not be produced", ERROR_GEN_FAILURE);
        }
    }

    [[nodiscard]] bool write_record(TraceChannelState& channel, const TraceEnvelope& envelope,
                                    std::span<const std::byte> payload) noexcept {
        write_definition(channel);
        if (failed_.load(std::memory_order_relaxed)) {
            return false;
        }
        const RecordHeader header{channel.channel_id, channel.patch.value, channel.version, 0, envelope};
        // Separate ETW descriptors for the header and opaque payload prevent ambiguous framing.
        std::array<EVENT_DATA_DESCRIPTOR, 2> data{};
        EventDataDescCreate(&data[0], &header, sizeof(header));
        EventDataDescCreate(&data[1], payload.data(), static_cast<ULONG>(payload.size()));
        constexpr EVENT_DESCRIPTOR descriptor{2, 1, 0, TRACE_LEVEL_VERBOSE, 0, 0, 0};
        const auto result = EventWrite(provider_handle_, &descriptor, static_cast<ULONG>(data.size()), data.data());
        if (result == ERROR_SUCCESS) {
            return true;
        }
        if (result == ERROR_DISK_FULL || result == ERROR_HANDLE_DISK_FULL) {
            limit_all();
        } else {
            fail_all("A trace record could not be written", result);
        }
        return false;
    }

    void drain_channel(TraceChannelState& channel) noexcept {
        // Consume the contiguous published prefix; a sequence mismatch means this channel currently has no record.
        for (;;) {
            const auto position = channel.dequeue_position;
            auto& slot = channel.slots[position % channel.capacity];
            const auto sequence = slot.sequence.load(std::memory_order_acquire);
            if (static_cast<std::int64_t>(sequence - (position + 1)) != 0) {
                return;
            }

            TraceEnvelope envelope{};
            std::memcpy(&envelope, slot.bytes, sizeof(envelope));
            const auto payload =
                std::span{slot.bytes + sizeof(envelope), static_cast<std::size_t>(envelope.payload_size)};
            const bool terminal =
                failed_.load(std::memory_order_relaxed) || limit_reached_.load(std::memory_order_relaxed);
            if (!terminal && open_trace() && write_record(channel, envelope, payload)) {
                increment_saturating(channel.written);
            } else {
                increment_saturating(channel.dropped);
            }
            // Advancing the sequence to the next ring generation lets a producer claim this fixed slot again.
            channel.dequeue_position = position + 1;
            slot.sequence.store(position + channel.capacity, std::memory_order_release);
        }
    }

    void update_file_limit() noexcept {
        // ETW may report a full file only on a later write, so periodic size checks terminalize producers promptly.
        const auto current_path = path();
        if (!current_path || owner_->max_trace_size_mb_ == 0) {
            return;
        }
        std::error_code error;
        const auto bytes = std::filesystem::file_size(*current_path, error);
        const auto limit = static_cast<std::uint64_t>(owner_->max_trace_size_mb_) * 1024U * 1024U;
        if (!error && bytes >= limit) {
            limit_all();
        }
    }

    // Periodic ETW metadata makes a retained file self-describing even when no later status snapshot is available.
    void write_health() noexcept {
        if (trace_handle_ == 0 || provider_handle_ == 0 || failed_.load(std::memory_order_relaxed) ||
            limit_reached_.load(std::memory_order_relaxed)) {
            return;
        }
        std::uint64_t accepted{};
        std::uint64_t written{};
        std::uint64_t dropped{};
        // Aggregate with saturation so metadata remains truthful even after an individual counter reaches its limit.
        for (const auto& channel : owner_->channels_) {
            const auto add = [](std::uint64_t left, std::uint64_t right) {
                return right > std::numeric_limits<std::uint64_t>::max() - left
                           ? std::numeric_limits<std::uint64_t>::max()
                           : left + right;
            };
            accepted = add(accepted, channel->accepted.load(std::memory_order_relaxed));
            written = add(written, channel->written.load(std::memory_order_relaxed));
            dropped = add(dropped, channel->dropped.load(std::memory_order_relaxed));
        }
        // Health uses the same ETW provider as records, so its write result terminalizes every producer consistently.
        try {
            const auto health = std::format(L"Fusion Cutter trace health: accepted={} written={} dropped={}", accepted,
                                            written, dropped);
            const auto result = EventWriteString(provider_handle_, TRACE_LEVEL_INFORMATION, 0, health.c_str());
            if (result == ERROR_DISK_FULL || result == ERROR_HANDLE_DISK_FULL) {
                limit_all();
            } else if (result != ERROR_SUCCESS) {
                fail_all("Trace health metadata could not be written", result);
            }
        } catch (...) {
            fail_all("Trace health metadata could not be produced", ERROR_GEN_FAILURE);
        }
    }

    void fail_all(std::string_view reason, DWORD error) noexcept {
        // One terminal error explains the failure of the shared session; subsequent channels observe it through health.
        if (!failed_.exchange(true, std::memory_order_relaxed)) {
            owner_->logger_.error("{} (Windows error {})", reason, error);
        }
    }

    void limit_all() noexcept {
        // File capacity is an expected terminal boundary, but the resulting producer drops are degraded behavior.
        if (!limit_reached_.exchange(true, std::memory_order_relaxed)) {
            owner_->logger_.warning("Trace output reached its {} MiB file limit; later records will be dropped",
                                    owner_->max_trace_size_mb_);
        }
    }

    void run() noexcept {
        // One low-priority consumer services every channel and marks each accepted record written or dropped.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const std::array handles{stop_, wake_};
        for (;;) {
            const auto wait = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, 1000);
            if (wait == WAIT_OBJECT_0) {
                const std::scoped_lock lock(owner_->channels_mutex_);
                for (auto& channel : owner_->channels_) {
                    if (channel->armed.load(std::memory_order_acquire)) {
                        drain_channel(*channel);
                    }
                }
                return;
            }
            if (wait == WAIT_FAILED) {
                // A broken wait primitive is a permanent writer failure; drain once so accepted records become drops.
                fail_all("Trace writer wait failed", GetLastError());
            }
            {
                // Channel preparation and release may wait for a drain; producers touch stable pointers and slots.
                const std::scoped_lock lock(owner_->channels_mutex_);
                for (auto& channel : owner_->channels_) {
                    if (channel->armed.load(std::memory_order_acquire)) {
                        drain_channel(*channel);
                    }
                }
                if (wait == WAIT_TIMEOUT) {
                    write_health();
                }
            }
            update_file_limit();
            if (wait == WAIT_FAILED) {
                return;
            }
        }
    }

    void close_trace() noexcept {
        // Provider registration is released before stopping its private session during orderly writer teardown.
        if (provider_handle_ != 0) {
            EventUnregister(provider_handle_);
            provider_handle_ = 0;
        }
        if (trace_handle_ != 0 && !properties_.empty()) {
            ControlTraceW(trace_handle_, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties_.data()),
                          EVENT_TRACE_CONTROL_STOP);
            trace_handle_ = 0;
        }
    }

    TraceSession* owner_{};
    HANDLE wake_{};
    HANDLE stop_{};
    std::thread thread_;
    TRACEHANDLE trace_handle_{};
    REGHANDLE provider_handle_{};
    std::vector<std::byte> properties_;
    mutable std::mutex path_mutex_;
    std::filesystem::path trace_path_;
    std::atomic_bool failed_{};
    std::atomic_bool limit_reached_{};
};

PreparedTraceChannel::PreparedTraceChannel(TraceSession& session, TraceChannelState& channel) noexcept
    : session_(&session), channel_(&channel) {}

PreparedTraceChannel::PreparedTraceChannel(PreparedTraceChannel&& other) noexcept
    : session_(std::exchange(other.session_, nullptr)), channel_(std::exchange(other.channel_, nullptr)) {}

PreparedTraceChannel& PreparedTraceChannel::operator=(PreparedTraceChannel&& other) noexcept {
    if (this != &other) {
        release();
        session_ = std::exchange(other.session_, nullptr);
        channel_ = std::exchange(other.channel_, nullptr);
    }
    return *this;
}

PreparedTraceChannel::~PreparedTraceChannel() {
    release();
}

void PreparedTraceChannel::release() noexcept {
    if (session_ != nullptr && channel_ != nullptr) {
        session_->release(*channel_);
    }
    session_ = nullptr;
    channel_ = nullptr;
}

TraceSession::TraceSession(std::uint32_t max_trace_size_mb) noexcept : max_trace_size_mb_(max_trace_size_mb) {}

TraceSession::~TraceSession() = default;

bool TraceSession::initialize_handle_table() noexcept {
    if (handle_slots_ != nullptr) {
        return true;
    }
    try {
        auto slots = std::make_unique<std::atomic<TraceChannelState*>[]>(kTraceHandleCapacity);
        auto generations = std::make_unique<std::atomic<std::uint32_t>[]>(kTraceHandleCapacity);
        auto free_ids = std::make_unique<std::uint32_t[]>(kTraceHandleCapacity);
        // Reserve the matching ownership entries inside the same aggregate budget so publication never reallocates.
        channels_.reserve(kTraceHandleCapacity);
        for (std::size_t index = 0; index < kTraceHandleCapacity; ++index) {
            slots[index].store(nullptr, std::memory_order_relaxed);
            generations[index].store(0, std::memory_order_relaxed);
        }
        handle_slots_ = std::move(slots);
        handle_generations_ = std::move(generations);
        free_handle_ids_ = std::move(free_ids);
        handle_capacity_ = kTraceHandleCapacity;
        return true;
    } catch (...) {
        return false;
    }
}

void TraceSession::configure(std::uint32_t max_trace_size_mb, std::filesystem::path installation_directory,
                             CoreLogger logger) noexcept {
    const std::scoped_lock lock(channels_mutex_);
    if (channels_.empty()) {
        max_trace_size_mb_ = max_trace_size_mb;
        installation_directory_ = std::move(installation_directory);
        logger_ = logger;
    }
}

void TraceSession::set_catalog(const catalog::Catalog& catalog) noexcept {
    catalog_ = &catalog;
}

FC_TraceCreateResult TraceSession::prepare_channel(catalog::PatchIndex patch, const FC_TraceDefinition* definition,
                                                   PreparedTraceChannel& preparation, FC_TraceHandle* output) noexcept {
    if (output != nullptr) {
        *output = nullptr;
    }
    // The native boundary validates the complete required prefix and every value needed to size bounded storage.
    constexpr auto required_size = offsetof(FC_TraceDefinition, version) + sizeof(definition->version);
    if (definition == nullptr || output == nullptr || definition->struct_size < required_size ||
        (definition->name.data == nullptr && definition->name.size != 0)) {
        return FC_TRACE_REJECTED;
    }
    const auto name = definition->name.size == 0 ? std::string_view{}
                                                 : std::string_view{definition->name.data, definition->name.size};
    if (!valid_name(name) || definition->capacity == 0 || definition->max_record_size == 0 ||
        definition->max_record_size > kMaximumRecordSize || definition->version == 0) {
        return FC_TRACE_REJECTED;
    }
    requested_.store(true, std::memory_order_relaxed);
    // Tracing disabled by configuration is a valid inert author value and therefore consumes no process budget.
    if (max_trace_size_mb_ == 0) {
        return FC_TRACE_DISABLED;
    }

    const auto bytes = storage_size(name, definition->capacity, definition->max_record_size);
    if (!bytes) {
        return FC_TRACE_REJECTED;
    }

    try {
        // Every ring slot reserves its registered maximum now so writes from a hook never allocate or resize storage.
        auto channel = std::make_unique<TraceChannelState>();
        channel->patch = patch;
        channel->name.assign(name);
        channel->capacity = definition->capacity;
        channel->maximum_record_size = definition->max_record_size;
        channel->version = definition->version;
        channel->storage_bytes = *bytes;
        channel->slots = std::make_unique<TraceSlot[]>(definition->capacity);
        const auto payload_size = sizeof(TraceEnvelope) + static_cast<std::size_t>(definition->max_record_size);
        constexpr auto alignment = alignof(std::uint64_t);
        channel->slot_stride = (payload_size + alignment - 1) & ~(alignment - 1);
        channel->payload_storage =
            std::make_unique<std::byte[]>(static_cast<std::size_t>(definition->capacity) * channel->slot_stride);
        for (std::uint32_t index = 0; index < definition->capacity; ++index) {
            channel->slots[index].sequence.store(index, std::memory_order_relaxed);
            channel->slots[index].bytes =
                channel->payload_storage.get() + static_cast<std::size_t>(index) * channel->slot_stride;
        }

        auto* handle = channel.get();
        {
            const std::scoped_lock lock(channels_mutex_);
            // A channel name must be unique within its patch across installed, retained, and prepared channels.
            if (std::ranges::any_of(channels_,
                                    [&](const auto& existing) {
                                        return existing->patch == patch && existing->name == name;
                                    }) ||
                *bytes > kTraceChannelStorageCapacity - reserved_bytes_ || !initialize_handle_table()) {
                return FC_TRACE_REJECTED;
            }
            if (free_handle_count_ == 0 && next_channel_id_ >= handle_capacity_) {
                return FC_TRACE_REJECTED;
            }
            // Keep a newly created writer attempt-local until channel storage and handle publication cannot fail.
            std::unique_ptr<Writer> prepared_writer;
            if (!writer_) {
                prepared_writer = std::make_unique<Writer>(*this);
                if (!prepared_writer->ready()) {
                    return FC_TRACE_REJECTED;
                }
            }
            channels_.push_back(std::move(channel));
            if (free_handle_count_ != 0) {
                handle->channel_id = free_handle_ids_[--free_handle_count_];
            } else {
                handle->channel_id = next_channel_id_++;
            }
            // Advancing without wrap prevents an abandoned token from naming a later channel in the recycled slot.
            const auto prior_generation = handle_generations_[handle->channel_id].load(std::memory_order_relaxed);
            handle->handle_generation = prior_generation + 1;
            reserved_bytes_ += *bytes;
            handle_generations_[handle->channel_id].store(handle->handle_generation, std::memory_order_release);
            handle_slots_[handle->channel_id].store(handle, std::memory_order_release);
            if (prepared_writer) {
                writer_ = std::move(prepared_writer);
            }
        }
        preparation = PreparedTraceChannel{*this, *handle};
        const auto token = (static_cast<std::uintptr_t>(handle->handle_generation) << kHandleIndexBits) |
                           (static_cast<std::uintptr_t>(handle->channel_id) + 1);
        *output = reinterpret_cast<FC_TraceHandle>(token);
        if (catalog_ != nullptr && patch.value < catalog_->patch_count()) {
            logger_.debug("Prepared trace channel '{}' for patch '{}' with {} slot(s) of up to {} byte(s)", name,
                          catalog_->patch(patch).id, definition->capacity, definition->max_record_size);
        } else {
            logger_.debug("Prepared trace channel '{}' for patch index {} with {} slot(s) of up to {} byte(s)", name,
                          patch.value, definition->capacity, definition->max_record_size);
        }
        return FC_TRACE_CREATED;
    } catch (...) {
        return FC_TRACE_REJECTED;
    }
}

void TraceSession::arm(PreparedTraceChannel& preparation) noexcept {
    if (preparation.session_ != this || preparation.channel_ == nullptr) {
        return;
    }
    preparation.channel_->armed.store(true, std::memory_order_release);
    // Clearing the token transfers channel lifetime entirely into this process-lifetime session.
    preparation.session_ = nullptr;
    preparation.channel_ = nullptr;
}

TraceChannelState* TraceSession::find(FC_TraceHandle handle) noexcept {
    return const_cast<TraceChannelState*>(std::as_const(*this).find(handle));
}

const TraceChannelState* TraceSession::find(FC_TraceHandle handle) const noexcept {
    const auto token = reinterpret_cast<std::uintptr_t>(handle);
    if (token == 0 || token > std::numeric_limits<std::uint32_t>::max() || handle_slots_ == nullptr) {
        return nullptr;
    }
    const auto slot_token = static_cast<std::uint32_t>(token) & kHandleIndexMask;
    const auto generation = static_cast<std::uint32_t>(token) >> kHandleIndexBits;
    if (slot_token == 0 || generation == 0 || slot_token - 1 >= handle_capacity_ ||
        handle_generations_[slot_token - 1].load(std::memory_order_acquire) != generation) {
        return nullptr;
    }
    const auto* channel = handle_slots_[slot_token - 1].load(std::memory_order_acquire);
    // Recheck after reading the independently published pointer so concurrent slot reuse cannot mix generations.
    if (handle_generations_[slot_token - 1].load(std::memory_order_acquire) != generation) {
        return nullptr;
    }
    return channel;
}

FC_Bool TraceSession::enabled(FC_TraceHandle handle) const noexcept {
    const auto* channel = find(handle);
    return channel != nullptr && channel->armed.load(std::memory_order_acquire) &&
                   !channel->file_limit_reached.load(std::memory_order_relaxed) &&
                   !channel->output_failed.load(std::memory_order_relaxed) && writer_ && !writer_->failed() &&
                   !writer_->limit_reached()
               ? FC_TRUE
               : FC_FALSE;
}

FC_Bool TraceSession::try_write(FC_TraceHandle handle, FC_ByteView record) noexcept {
    auto* channel = find(handle);
    // Malformed payloads are contract errors rather than capacity drops and therefore do not change health counters.
    if (channel == nullptr || record.data == nullptr || record.size == 0 ||
        record.size > channel->maximum_record_size) {
        return FC_FALSE;
    }
    if (!channel->armed.load(std::memory_order_acquire)) {
        return FC_FALSE;
    }
    // Once an armed stream becomes terminal, every otherwise valid rejected producer record is counted as lost.
    if (channel->file_limit_reached.load(std::memory_order_relaxed) ||
        channel->output_failed.load(std::memory_order_relaxed) || !writer_ || writer_->failed() ||
        writer_->limit_reached()) {
        increment_saturating(channel->dropped);
        return FC_FALSE;
    }

    // Claim one free ring position; a full queue returns immediately without waiting for the reporting consumer.
    auto position = channel->enqueue_position.load(std::memory_order_relaxed);
    TraceSlot* slot{};
    bool claimed = false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        slot = &channel->slots[position % channel->capacity];
        const auto sequence = slot->sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::int64_t>(sequence - position);
        if (difference == 0 &&
            channel->enqueue_position.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
            claimed = true;
            break;
        }
        if (difference < 0) {
            increment_saturating(channel->dropped);
            return FC_FALSE;
        }
        position = channel->enqueue_position.load(std::memory_order_relaxed);
    }
    if (!claimed) {
        increment_saturating(channel->dropped);
        return FC_FALSE;
    }

    // The envelope is captured on this producer before release publication, preserving real emission facts.
    const TraceEnvelope envelope{producer_timestamp(), position, GetCurrentThreadId(), record.size};
    std::memcpy(slot->bytes, &envelope, sizeof(envelope));
    std::memcpy(slot->bytes + sizeof(envelope), record.data, record.size);
    increment_saturating(channel->accepted);
    slot->sequence.store(position + 1, std::memory_order_release);
    notify_writer();
    return FC_TRUE;
}

void TraceSession::health(FC_TraceHandle handle, FC_TraceHealth* output) const noexcept {
    if (output == nullptr) {
        return;
    }
    // An undersized output has no generation-1 required prefix and therefore remains untouched.
    constexpr auto required_size = offsetof(FC_TraceHealth, output_failed) + sizeof(output->output_failed);
    const auto requested_size = output->struct_size;
    if (requested_size < required_size) {
        return;
    }
    // Copy only the caller-declared prefix so older SDK consumers remain compatible with later table tails.
    FC_TraceHealth snapshot{.struct_size = sizeof(FC_TraceHealth)};
    if (const auto* channel = find(handle)) {
        snapshot.accepted = channel->accepted.load(std::memory_order_relaxed);
        snapshot.written = channel->written.load(std::memory_order_relaxed);
        snapshot.dropped = channel->dropped.load(std::memory_order_relaxed);
        snapshot.file_limit_reached =
            (channel->file_limit_reached.load(std::memory_order_relaxed) || (writer_ && writer_->limit_reached()))
                ? FC_TRUE
                : FC_FALSE;
        snapshot.output_failed =
            (channel->output_failed.load(std::memory_order_relaxed) || (writer_ && writer_->failed())) ? FC_TRUE
                                                                                                       : FC_FALSE;
    }
    std::memcpy(output, &snapshot, std::min<std::size_t>(requested_size, sizeof(snapshot)));
}

void TraceSession::release(TraceChannelState& channel) noexcept {
    // Declaring the writer before the lock keeps its potentially blocking destructor outside the critical section.
    std::unique_ptr<Writer> released_writer;
    const std::scoped_lock lock(channels_mutex_);
    const auto found = std::ranges::find_if(channels_, [&](const auto& candidate) {
        return candidate.get() == &channel;
    });
    // Armed channels are permanent; only an unpublished channel from the Prepare phase can return its reservation.
    if (found == channels_.end() || channel.armed.load(std::memory_order_acquire)) {
        return;
    }
    handle_slots_[channel.channel_id].store(nullptr, std::memory_order_release);
    // A slot at its maximum generation is retired rather than allowing any previously issued token to recur.
    if (channel.handle_generation != kHandleIndexMask) {
        free_handle_ids_[free_handle_count_++] = channel.channel_id;
    }
    reserved_bytes_ -= channel.storage_bytes;
    channels_.erase(found);
    if (channels_.empty()) {
        // The writer has not opened a file because no released channel was ever armed.
        released_writer = std::move(writer_);
    }
}

std::size_t TraceSession::channel_count() const noexcept {
    const std::scoped_lock lock(channels_mutex_);
    return channels_.size();
}

std::size_t TraceSession::reserved_bytes() const noexcept {
    const std::scoped_lock lock(channels_mutex_);
    return reserved_bytes_;
}

void TraceSession::notify_writer() noexcept {
    if (writer_) {
        writer_->notify();
    }
}

reporting::TraceStatus TraceSession::status() const noexcept {
    reporting::TraceStatus result{.requested = requested_.load(std::memory_order_relaxed),
                                  .configured_disabled =
                                      requested_.load(std::memory_order_relaxed) && max_trace_size_mb_ == 0};
    const std::scoped_lock lock(channels_mutex_);
    if (writer_) {
        const auto writer_status = writer_->status();
        result.path = writer_status.path;
        result.file_limit_reached = writer_status.file_limit_reached;
        result.output_failed = writer_status.output_failed;
    }
    for (const auto& channel : channels_) {
        const auto dropped = channel->dropped.load(std::memory_order_relaxed);
        result.dropped = dropped > std::numeric_limits<std::uint64_t>::max() - result.dropped
                             ? std::numeric_limits<std::uint64_t>::max()
                             : result.dropped + dropped;
    }
    return result;
}

} // namespace fc::runtime
