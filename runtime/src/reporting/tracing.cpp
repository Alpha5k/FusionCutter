#include "tracing.hpp"

#include "trace_channel.hpp"

#include "reporting.hpp"
#include "bounded_text.hpp"
#include "patches/patch_record.hpp"

#include <fc/base.hpp>

#include <Windows.h>
#include <evntrace.h>
#include <evntprov.h>
#include <evntcons.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <format>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fc::runtime {

using namespace trace_detail;

namespace {

// Identifies Fusion Cutter records and string metadata to ETW consumers.
constexpr GUID kProvider = {0xd5645019, 0xaf63, 0x46b9, {0x93, 0xc8, 0x91, 0x0f, 0x22, 0xa7, 0x01, 0x51}};

using fc::detail::view;

constexpr std::uint64_t kMebibyte = 1024U * 1024U;

// Rounds enabled file budgets to ETW's whole-MiB units; zero remains disabled.
constexpr std::uint64_t trace_mebibytes(std::uint64_t bytes) noexcept {
    return bytes / kMebibyte + (bytes % kMebibyte != 0);
}

// Converts UTF-8 metadata to the UTF-16 text expected by EventWriteString; invalid input throws.
std::wstring wide(std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("Trace text is too long for Windows UTF-8 conversion");
    }

    // Query the required character count before allocating the destination.
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        throw std::runtime_error("Trace text could not be converted from UTF-8");
    }

    std::wstring result(static_cast<std::size_t>(length), 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

} // namespace

// Holds the worker's ETW resources and the channel objects shared with producers and status readers.
struct TraceSession::State {
    // Drains and joins the worker before releasing ETW resources, its wake event and owned channels.
    ~State() {
        shutdown();
        if (wake) {
            CloseHandle(wake);
        }
    }

    // ETW file limit in whole MiB; zero leaves newly registered channels disabled.
    std::uint64_t maximum_mib = 512;
    // The worker creates the traces subdirectory here when it first needs output.
    std::filesystem::path directory;
    // Optional diagnostic destination supplied before channel creation; it must remain alive until shutdown.
    reporting::ReportingSession *diagnostics{};
    // Protect channel registration/iteration and publication of the output path, respectively.
    mutable std::mutex channels_mutex, path_mutex;
    std::mutex shutdown_mutex;
    // Objects remain at stable addresses even when this vector grows or an owner's queue is discarded.
    std::vector<std::unique_ptr<TraceChannel>> channels;
    // Published only after ETW startup and session metadata succeed.
    std::filesystem::path path;
    // Auto-reset event shared by all producers to request another worker pass.
    HANDLE wake{};
    std::thread worker;
    // Closed admission starts the final drain; failure and limit flags stop output and remain visible to producers.
    std::atomic_bool failed{}, limited{}, admission_closed{};
    // ETW's cumulative losses from the most recent successful health query.
    std::atomic<std::uint64_t> events_lost{}, buffers_lost{};
    // ETW resources are used by the worker and closed after it joins.
    TRACEHANDLE session{};
    REGHANDLE provider{};
    // One Windows properties block followed by the logger name and file path, both null-terminated UTF-16.
    std::vector<std::byte> properties;
    // Only the worker changes this flag; ETW reports session losses without channel attribution.
    bool etw_warning_written{};

    // Serializes explicit finalization while retaining channel storage for later callers.
    void shutdown() noexcept {
        std::scoped_lock shutdown_lock(shutdown_mutex);
        {
            // Registration holds the same lock before starting a worker or publishing a channel.
            std::scoped_lock channels_lock(channels_mutex);
            admission_closed.store(true);
            if (wake) {
                SetEvent(wake);
            }
        }
        if (worker.joinable()) {
            worker.join();
        }
        close();
    }

    // Sends an output diagnostic to the configured reporter or the Windows debugger.
    void diagnostic(std::string_view reason, DWORD code, FC_LogLevel level = FC_LOG_ERROR) noexcept {
        if (diagnostics) {
            diagnostics->diagnostic(reason, code, level);
        } else {
            // Standalone sessions can still explain output failures through the Windows debugger.
            std::array<char, 1024> text{};
            const auto prefix = static_cast<std::size_t>(
                _snprintf_s(text.data(), text.size(), _TRUNCATE, "[%s] [Fusion Cutter] ", level == FC_LOG_WARNING ? "Warning" : "Error"));
            constexpr std::string_view marker = " [truncated]";
            const bool truncated = reason.size() > 900;
            auto size =
                prefix + reporting::copy_structured_bytes(std::span{text}.subspan(prefix, truncated ? 900 - marker.size() : 900), reason);
            if (truncated) {
                std::memcpy(text.data() + size, marker.data(), marker.size());
                size += marker.size();
            }
            const auto suffix = code ? _snprintf_s(text.data() + size, text.size() - size, _TRUNCATE, " (Windows error %lu)\r\n",
                                                   static_cast<unsigned long>(code))
                                     : _snprintf_s(text.data() + size, text.size() - size, _TRUNCATE, "\r\n");
            std::array<wchar_t, 1024> wide{};
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(size + (suffix > 0 ? suffix : 0)), wide.data(),
                                static_cast<int>(wide.size() - 1));
            OutputDebugStringW(wide.data());
        }
    }

    // Set the failure or limit flag and report it once.
    void terminal(bool limit, std::string_view reason, DWORD code) noexcept {
        const bool already = limit ? limited.exchange(true) : failed.exchange(true);
        if (!already) {
            diagnostic(reason, code, limit ? FC_LOG_WARNING : FC_LOG_ERROR);
        }
    }

    // Returns whether ETW succeeded; failures set the session's terminal output state and report a diagnostic.
    bool check(ULONG code, std::string_view reason) noexcept {
        if (code == ERROR_SUCCESS) {
            return true;
        }

        // Treat disk exhaustion as a file limit so health distinguishes it from other output failures.
        if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) {
            terminal(true, "Trace recording stopped because Windows reported a full disk", code);
        } else {
            // Sequential ETW sessions disappear when their compressed file reaches the native size limit.
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (code == ERROR_WMI_INSTANCE_NOT_FOUND && !path.empty() &&
                GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
                const auto bytes = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32U) | attributes.nFileSizeLow;
                if (bytes >= maximum_mib * kMebibyte) {
                    terminal(true, "Trace recording stopped at the Windows ETW file size limit", 0);
                    return false;
                }
            }
            terminal(false, reason, code);
        }
        return false;
    }

    // Starts ETW output when the worker processes its first record.
    bool open() noexcept {
        if (session && provider) {
            return true;
        }

        try {
            const auto traces = directory / "traces";
            std::error_code error;
            std::filesystem::create_directories(traces, error);
            if (error) {
                terminal(false, "The trace directory could not be created", static_cast<DWORD>(error.value()));
                return false;
            }

            // Time, process ID and session address distinguish files and private logger names.
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const auto name = std::format(L"FusionCutter-{}-{:x}", GetCurrentProcessId(), reinterpret_cast<std::uintptr_t>(this));
            const auto file = traces / std::format(L"FusionCutter-{:04}{:02}{:02}-{:02}{:02}{:02}.{:03}-{}-{:x}.etl", now.wYear, now.wMonth,
                                                   now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId(),
                                                   reinterpret_cast<std::uintptr_t>(this));

            // ETW locates the two strings by byte offsets within its properties allocation.
            const auto name_bytes = (name.size() + 1) * sizeof(wchar_t);
            const auto file_bytes = (file.native().size() + 1) * sizeof(wchar_t);
            const auto total = sizeof(EVENT_TRACE_PROPERTIES) + name_bytes + file_bytes;
            if (total > std::numeric_limits<ULONG>::max()) {
                terminal(false, "The trace session properties, logger name and file path exceed the Windows 32-bit size limit",
                         ERROR_BUFFER_OVERFLOW);
                return false;
            }

            properties.assign(total, std::byte{});
            auto *config = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(properties.data());
            config->Wnode.BufferSize = static_cast<ULONG>(total);
            config->Wnode.Guid = kProvider;
            config->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            // Use performance-counter timestamps, matching the timestamps captured in producer envelopes.
            config->Wnode.ClientContext = 1;
            // ETW expresses BufferSize in KiB; request two 64 KiB buffers for this private session.
            config->BufferSize = 64;
            config->MinimumBuffers = 2;
            config->MaximumBuffers = 2;

            // ETW limits the compressed file in whole MiB; its final buffer can extend just beyond the limit.
            if (maximum_mib > std::numeric_limits<ULONG>::max()) {
                terminal(false, "The trace file limit cannot be represented by Windows ETW", ERROR_ARITHMETIC_OVERFLOW);
                return false;
            }

            config->MaximumFileSize = static_cast<ULONG>(maximum_mib);
            // Record this process's events sequentially and ask ETW to flush buffered output every second.
            config->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_PRIVATE_LOGGER_MODE | EVENT_TRACE_PRIVATE_IN_PROC |
                                  EVENT_TRACE_COMPRESSED_MODE;
            config->FlushTimer = 1;
            config->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            config->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + name_bytes);
            std::memcpy(properties.data() + config->LoggerNameOffset, name.c_str(), name_bytes);
            std::memcpy(properties.data() + config->LogFileNameOffset, file.c_str(), file_bytes);

            // Register before starting and enabling the logger; shutdown releases resources retained by partial startup.
            if (!check(EventRegister(&kProvider, nullptr, nullptr, &provider), "The trace provider could not be registered") ||
                !check(StartTraceW(&session, name.c_str(), config), "The private trace session could not be started") ||
                !check(EnableTraceEx2(session, &kProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, 0, 0, 0, nullptr),
                       "The trace provider could not be enabled")) {
                return false;
            }

            // Session metadata identifies the version and supplies the frequency needed to interpret producer timestamps.
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            const auto metadata = wide(std::format("Fusion Cutter {}; PID {}; architecture {}; QPC frequency {}", FC_VERSION_STRING,
                                                   GetCurrentProcessId(), sizeof(void *) * 8, frequency.QuadPart));
            if (!check(EventWriteString(provider, TRACE_LEVEL_INFORMATION, 0, metadata.c_str()),
                       "Trace session metadata could not be written")) {
                return false;
            }

            // Status readers see the path only once the file and its identifying metadata are ready.
            {
                std::scoped_lock lock(path_mutex);
                path = file;
            }

            return true;
        } catch (...) {
            terminal(false, "Trace output could not be initialized", ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
    }

    // Emit a channel's identity and payload format version before its first binary record.
    bool definition(TraceChannel::State &channel) noexcept {
        if (channel.definition_written) {
            return true;
        }

        try {
            // The state address links records to this metadata; owner declarations provide plugin and patch names.
            const auto *owner = channel.owner;
            const auto *plugin = owner ? owner->plugin->admitted.definition : nullptr;
            const auto text = wide(std::format("Channel {:x}: {}/{} plugin_version={} name={} format_version={} buffer_bytes={}",
                                               reinterpret_cast<std::uintptr_t>(&channel), plugin ? view(plugin->id) : "FusionCutter",
                                               owner ? view(owner->definition->id) : "Runtime", plugin ? view(plugin->version) : "",
                                               channel.name, channel.version, channel.queue.capacity));
            if (!check(EventWriteString(provider, TRACE_LEVEL_INFORMATION, 0, text.c_str()),
                       "Trace channel metadata could not be written")) {
                return false;
            }

            // Only the worker touches this flag, and only successful metadata output sets it.
            channel.definition_written = true;
            return true;
        } catch (...) {
            terminal(false, "Trace channel metadata could not be formatted", ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
    }

    // Submit one extracted record to ETW; false makes the drain count it as an output loss.
    bool write(TraceChannel::State &channel, const Envelope &envelope, std::uint64_t sequence,
               std::span<const std::byte> payload) noexcept {
        if (failed.load() || limited.load() || !open() || !definition(channel)) {
            return false;
        }

        // Prefix the opaque payload with enough information to identify its channel and original producer call.
        struct Header {
            // Matches the channel state address in the channel metadata string.
            std::uintptr_t channel;
            // Author-supplied version of the opaque payload format.
            std::uint32_t version;
            // Captured timestamp and thread, ordered channel sequence and payload byte count.
            std::uint64_t timestamp, sequence;
            std::uint32_t thread, payload_size;
        };

        // ETW concatenates the header and payload descriptors into one event's user data.
        const Header header{reinterpret_cast<std::uintptr_t>(&channel),
                            channel.version,
                            envelope.timestamp,
                            sequence,
                            envelope.thread,
                            envelope.payload_size};
        std::array<EVENT_DATA_DESCRIPTOR, 2> data{};
        EventDataDescCreate(&data[0], &header, sizeof(header));
        EventDataDescCreate(&data[1], payload.data(), static_cast<ULONG>(payload.size()));
        // Event ID 2, version 1 identifies this binary record layout to the decoder.
        constexpr EVENT_DESCRIPTOR descriptor{2, 1, 0, TRACE_LEVEL_VERBOSE, 0, 0, 0};
        if (!check(EventWrite(provider, &descriptor, static_cast<ULONG>(data.size()), data.data()),
                   "A trace record could not be written")) {
            return false;
        }

        return true;
    }

    // Processes a bounded batch; true requests another pass only when the next record is complete.
    bool drain(TraceChannel &channel, std::span<std::byte> payload, unsigned maximum_records) noexcept {
        auto &state = *channel.state_;
        // Discarded queues have one consumer too; wait for copying producers to leave before reclaiming storage.
        if (state.disabled.load() && state.active_writes.load() != 0) {
            return false;
        }
        for (unsigned i = 0; i != maximum_records; ++i) {
            const auto record = state.queue.next();
            if (!record) {
                if (state.disabled.load() && state.active_writes.load() == 0) {
                    // A producer may have published between the first empty check and its final departure.
                    if (state.queue.next()) {
                        return true;
                    }
                    state.queue.reclaim();
                }
                return false;
            }
            Envelope envelope{};
            state.queue.copy_out(*record, 0, &envelope, sizeof(envelope));
            if (state.disabled.load()) {
                state.queue.release(*record);
                state.dropped.fetch_add(1);
                continue;
            }

            // Return queue capacity before ETW work; sequences follow complete records in reservation order.
            state.queue.copy_out(*record, sizeof(envelope), payload.data(), envelope.payload_size);
            const auto sequence = state.sequence++;
            state.queue.release(*record);
            if (write(state, envelope, sequence, payload.first(envelope.payload_size))) {
                state.written.fetch_add(1);
            } else {
                state.dropped.fetch_add(1);
            }
        }

        return state.queue.next().has_value();
    }

    // Queues an individual warning so simultaneous channel explanations survive the reporter's single diagnostic slot.
    void warning(std::string_view reason) noexcept {
        if (!diagnostics || !diagnostics->write(nullptr, FC_LOG_WARNING, {reason.data(), static_cast<std::uint32_t>(reason.size())})) {
            diagnostic(reason, 0, FC_LOG_WARNING);
        }
    }

    // Reports each channel's first capacity loss independently of ETW startup or successful health queries.
    void queue_warnings() noexcept {
        try {
            std::scoped_lock lock(channels_mutex);
            for (const auto &channel : channels) {
                auto &state = *channel->state_;
                const auto count = state.queue_full.load();
                if (!count || state.queue_warning_written) {
                    continue;
                }
                const auto *owner = state.owner;
                const auto *plugin = owner ? owner->plugin->admitted.definition : nullptr;
                warning(std::format("Trace channel '{}/{}: {}' dropped {} record{} because its buffer was full. "
                                    "Further losses are counted in trace health.",
                                    plugin ? view(plugin->id) : "FusionCutter", owner ? view(owner->definition->id) : "Runtime", state.name,
                                    count, count == 1 ? "" : "s"));
                state.queue_warning_written = true;
            }
        } catch (...) {
            diagnostic("Trace channel loss warning could not be prepared", ERROR_NOT_ENOUGH_MEMORY);
        }
    }

    // Queries ETW losses and emits the existing aggregate channel health metadata.
    void health() noexcept {
        queue_warnings();
        if (!session || failed.load() || limited.load()) {
            return;
        }

        // Query ETW's losses separately from records refused by the runtime's queues or output path.
        auto *config = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(properties.data());
        // ControlTrace reuses this block; restore its required flags and request no file-name change.
        config->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        config->LogFileNameOffset = 0;
        const auto code = ControlTraceW(session, nullptr, config, EVENT_TRACE_CONTROL_QUERY);
        const auto reason = code == ERROR_WMI_INSTANCE_NOT_FOUND
                                ? "Trace recording stopped unexpectedly; no further records will be written"
                                : "Trace recording health could not be queried";
        if (!check(code, reason)) {
            return;
        }

        events_lost.store(config->EventsLost);
        buffers_lost.store(static_cast<std::uint64_t>(config->LogBuffersLost) + config->RealTimeBuffersLost);

        try {
            if (!etw_warning_written && (events_lost.load() || buffers_lost.load())) {
                warning(std::format("Windows ETW trace losses: events={} buffers={}. "
                                    "Further losses are counted in trace health.",
                                    events_lost.load(), buffers_lost.load()));
                etw_warning_written = true;
            }
            // Aggregate the channels' atomic counters; submissions can continue while these snapshots are collected.
            std::uint64_t accepted{}, written{}, dropped{};
            {
                std::scoped_lock lock(channels_mutex);
                for (const auto &channel : channels) {
                    const auto value = channel->health();
                    accepted += value.accepted;
                    written += value.written;
                    dropped += value.dropped;
                }
            }

            // Put runtime and ETW loss counts in the file so consumers can assess missing records.
            const auto text = std::format(L"Trace health: accepted={} written={} dropped={} ETW events_lost={} buffers_lost={}", accepted,
                                          written, dropped, events_lost.load(), buffers_lost.load());
            check(EventWriteString(provider, TRACE_LEVEL_INFORMATION, 0, text.c_str()), "Trace health metadata could not be written");

        } catch (...) {
            diagnostic("Trace health metadata could not be prepared", ERROR_NOT_ENOUGH_MEMORY);
        }
    }

    // Drains fair channel batches and emits periodic health until shutdown empties the queues or the wait fails.
    void run() noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        std::array<std::byte, kMaximumPayload> payload{};
        auto next_health = std::chrono::steady_clock::now() + std::chrono::seconds{1};

        for (;;) {
            // Producers request a pass through the event; the timeout also allows idle health checks.
            const auto waited = WaitForSingleObject(wake, 1000);
            if (waited == WAIT_FAILED) {
                terminal(false, "The trace worker event wait failed", GetLastError());
            }

            // Snapshot the channel count; registrations added during this pass are visited on a later pass.
            bool pending{};
            std::size_t count{};
            {
                std::scoped_lock lock(channels_mutex);
                count = channels.size();
            }

            for (std::size_t index = 0; index != count; ++index) {
                // The vector can grow after unlocking, but the separately allocated channel stays alive.
                TraceChannel *channel{};
                {
                    std::scoped_lock lock(channels_mutex);
                    channel = channels[index].get();
                }

                // Limit each channel to sixteen records so a busy channel leaves time for other channels.
                pending |= drain(*channel, payload, 16);
            }

            // Busy producers can keep the event signaled; health opportunities follow elapsed time independently.
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_health) {
                health();
                next_health = now + std::chrono::seconds{1};
            }

            // Schedule another pass immediately when a batch leaves records behind.
            if (pending) {
                SetEvent(wake);
            }

            // After observing stop, wait for admitted producers before checking queues again: a producer may have
            // published after this pass's earlier empty check. Closed admission prevents new accepted reservations.
            if (admission_closed.load() && !pending) {
                bool active{}, queued{};
                {
                    std::scoped_lock lock(channels_mutex);
                    for (const auto &channel : channels) {
                        active |= channel->state_->active_writes.load() != 0;
                    }
                    if (!active) {
                        for (const auto &channel : channels) {
                            queued |= channel->state_->queue.next().has_value();
                        }
                    }
                }
                if (!active && !queued) {
                    health();
                    break;
                }
                // An active producer wakes the worker when it leaves; unfinished reservations do not need polling.
                if (queued) {
                    SetEvent(wake);
                }
            }
            if (waited == WAIT_FAILED) {
                break;
            }
        }
    }

    // Called after the worker joins; also releases handles left by a partially completed startup.
    void close() noexcept {
        if (provider) {
            EventUnregister(provider);
            provider = 0;
        }

        // Stopping the logger flushes ETW's remaining buffered events and finalizes the sequential file.
        if (session && !properties.empty()) {
            auto *config = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(properties.data());
            config->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            config->LogFileNameOffset = 0;
            check(ControlTraceW(session, nullptr, config, EVENT_TRACE_CONTROL_STOP), "The private trace session could not be stopped");
            session = 0;
        }
    }
};

TraceSession::TraceSession(std::uint64_t maximum) : state_(std::make_unique<State>()) {
    state_->maximum_mib = trace_mebibytes(maximum);
}

TraceSession::~TraceSession() = default;

void TraceSession::shutdown() noexcept {
    state_->shutdown();
}

void TraceSession::configure(std::uint64_t maximum, std::filesystem::path directory, reporting::ReportingSession *diagnostics) noexcept {
    state_->maximum_mib = trace_mebibytes(maximum);
    state_->directory = std::move(directory);
    state_->diagnostics = diagnostics;
}

std::expected<TraceChannel *, Diagnostic> TraceSession::create_channel(const PatchRecord *owner, FC_TraceDefinition definition) noexcept {
    try {
        if (!definition.name.data || !definition.name.size ||
            definition.name.size > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || !definition.buffer_bytes ||
            definition.buffer_bytes > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(Diagnostic{"Trace", "The trace channel name or buffer size in bytes is invalid", 0});
        }

        // Names appear in metadata, so reject invalid UTF-8 and ASCII control characters before retaining them.
        const auto name = view(definition.name);
        if (std::any_of(name.begin(), name.end(),
                        [](unsigned char c) {
                            return c < 0x20 || c == 0x7f;
                        }) ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0) <= 0) {
            return std::unexpected(
                Diagnostic{"Trace", "The trace channel name contains control characters or could not be converted from UTF-8", 0});
        }

        // Serialize duplicate checks and insertion; different owners may use the same channel name.
        std::scoped_lock lock(state_->channels_mutex);
        if (state_->admission_closed.load()) {
            return std::unexpected(Diagnostic{"Trace", "The trace session no longer accepts channels", 0});
        }
        for (const auto &channel : state_->channels) {
            if (channel->state_->owner == owner && channel->state_->name == name) {
                return std::unexpected(Diagnostic{"Trace", "This owner already has a trace channel with that name", 0});
            }
        }

        // Copy the declaration and share the failure and limit flags so producers can check whether output is available.
        auto channel = std::make_unique<TraceChannel::State>();
        channel->owner = owner;
        channel->name = name;
        channel->version = definition.format_version;
        channel->disabled = state_->maximum_mib == 0;
        channel->output_failed = &state_->failed;
        channel->file_limit_reached = &state_->limited;
        channel->admission_closed = &state_->admission_closed;

        // Disabled channels still register, but only enabled channels need queue storage and the shared worker.
        if (!channel->disabled) {
            channel->queue.initialize(static_cast<std::size_t>(definition.buffer_bytes));
            if (!state_->wake) {
                // Auto-reset combines arrivals from all channels into requests for worker passes.
                state_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!state_->wake) {
                    return std::unexpected(Diagnostic{"Trace", "The trace worker event could not be created", GetLastError()});
                }
            }
            if (!state_->worker.joinable()) {
                state_->worker = std::thread{[state = state_.get()] {
                    state->run();
                }};
            }
            channel->wake = state_->wake;
        }

        // Publish ownership before returning the stable channel pointer to its producer.
        auto result = std::unique_ptr<TraceChannel>(new TraceChannel(std::move(channel)));
        auto *pointer = result.get();
        state_->channels.push_back(std::move(result));
        return pointer;
    } catch (...) {
        return std::unexpected(Diagnostic{"Trace", "The trace channel could not be created", ERROR_NOT_ENOUGH_MEMORY});
    }
}

reporting::TraceStatus TraceSession::status() const noexcept {
    // Producers and the worker can update these independently; this is not one atomic session snapshot.
    reporting::TraceStatus result;
    result.configured_disabled = !state_->maximum_mib;
    result.output_failed = state_->failed.load();
    result.file_limit_reached = state_->limited.load();
    result.events_lost = state_->events_lost.load();
    result.buffers_lost = state_->buffers_lost.load();

    try {
        // Derive request history and losses from retained channels before the output path copy can allocate.
        {
            std::scoped_lock lock(state_->channels_mutex);
            result.requested = !state_->channels.empty();
            for (const auto &channel : state_->channels) {
                result.dropped += channel->health().dropped;
            }
        }

        std::scoped_lock lock(state_->path_mutex);
        if (!state_->path.empty()) {
            result.path = state_->path;
        }
    } catch (...) {
        result.output_failed = true;
    }

    return result;
}

void TraceSession::discard(const PatchRecord *owner) noexcept {
    std::scoped_lock channels_lock(state_->channels_mutex);
    for (auto &channel : state_->channels) {
        auto &record = *channel->state_;
        if (record.owner != owner) {
            continue;
        }

        // The worker accounts for remaining records and reclaims storage after admitted producers have left.
        record.disabled.store(true);
    }
    if (state_->wake) {
        SetEvent(state_->wake);
    }
}

} // namespace fc::runtime
