#pragma once

#include <fc/base.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace fc {

enum class LogLevel : std::uint32_t {
    Off = FC_LOG_OFF,
    Error = FC_LOG_ERROR,
    Warning = FC_LOG_WARNING,
    Info = FC_LOG_INFO,
    Debug = FC_LOG_DEBUG
};

namespace detail {

inline std::atomic<const FC_ReportingApi *> fatal_reporting{};

// Save the runtime's reporting table for fatal errors raised from this plugin DLL.
// The first available table wins and must remain valid until process exit.
inline void bind_reporting(const FC_Host *host) noexcept {
    if (!table_available(host, offsetof(FC_Host, reporting), sizeof(host->reporting)) || !host->reporting) {
        return;
    }
    const FC_ReportingApi *empty{};
    fatal_reporting.compare_exchange_strong(empty, host->reporting);
}

// Returns the host's reporting table when its ABI prefix includes it; otherwise returns null.
inline const FC_ReportingApi *reporting(StageAccess access) noexcept {
    if (!table_available(access.host, offsetof(FC_Host, reporting), sizeof(access.host->reporting))) {
        return nullptr;
    }
    return access.host->reporting;
}

template <class T>
inline constexpr bool log_text =
    std::same_as<std::remove_cvref_t<T>, std::string> || std::same_as<std::remove_cvref_t<T>, std::string_view> ||
    (std::is_pointer_v<std::remove_reference_t<T>> &&
     std::same_as<std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>, char>) ||
    (std::is_array_v<std::remove_reference_t<T>> && std::same_as<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, char>);
template <class T>
concept LogArgument = log_text<T> || std::integral<std::remove_cvref_t<T>> || std::floating_point<std::remove_cvref_t<T>>;
template <class T> using LogValue = std::conditional_t<log_text<T>, std::string_view, std::remove_cvref_t<T>>;

// Appends bytes, advancing position only on success; position must be within output.
inline bool copy_log(std::span<std::uint8_t> output, std::size_t &position, const void *source, std::size_t size) noexcept {
    if (size > output.size() - position) {
        return false;
    }
    if (size) {
        std::memcpy(output.data() + position, source, size);
    }
    position += size;
    return true;
}

// Copies text with its byte count, or a scalar's bytes, while the caller's arguments are still valid.
template <class T> bool encode_log(std::span<std::uint8_t> output, std::size_t &position, T &&argument) noexcept {
    if constexpr (log_text<T>) {
        std::string_view text;
        if constexpr (std::is_pointer_v<std::remove_reference_t<T>>) {
            text = argument ? std::string_view{argument} : std::string_view{};
        } else {
            text = std::string_view{argument};
        }
        if (text.size() > UINT32_MAX) {
            return false;
        }
        const auto size = static_cast<std::uint32_t>(text.size());
        return copy_log(output, position, &size, sizeof(size)) && copy_log(output, position, text.data(), text.size());
    } else {
        const LogValue<T> value = argument;
        return copy_log(output, position, &value, sizeof(value));
    }
}

// Reads the next typed value within the packet bounds; text borrows packet storage for the formatter call.
template <class T> bool decode_log(std::span<const std::uint8_t> &remaining, T &value) noexcept {
    if constexpr (std::same_as<T, std::string_view>) {
        std::uint32_t size{};
        if (!decode_log(remaining, size) || size > remaining.size()) {
            return false;
        }
        value = {reinterpret_cast<const char *>(remaining.data()), size};
        remaining = remaining.subspan(size);
    } else {
        if (sizeof(T) > remaining.size()) {
            return false;
        }
        // Scalar bytes may be unaligned in the packet, so decode through memcpy.
        std::memcpy(&value, remaining.data(), sizeof(T));
        remaining = remaining.subspan(sizeof(T));
    }
    return true;
}

// Reconstructs the typed arguments and formats them when the runtime worker invokes this callback.
// packet must contain the unmodified bytes encoded by Logger for this argument list.
template <class... Args> std::uint32_t FC_CDECL format_log(FC_Bytes packet, FC_TextSink sink) {
    if (!packet.data || !sink.write) {
        return 0;
    }
    std::span<const std::uint8_t> remaining{packet.data, packet.size};
    std::string_view format;
    std::tuple<LogValue<Args>...> values;
    if (!decode_log(remaining, format)) {
        return 0;
    }
    // Decode in the declared argument order; text continues to borrow the packet storage.
    const bool decoded = std::apply(
        [&](auto &...value) {
            return (decode_log(remaining, value) && ...);
        },
        values);
    if (!decoded || !remaining.empty()) {
        return 0;
    }

    try {
        const auto text = std::apply(
            [&](auto &...value) {
                return std::vformat(format, std::make_format_args(value...));
            },
            values);
        return sink.write(sink.context, detail::text(text));
    } catch (...) {
        return 0;
    }
}
} // namespace detail

// Submits text or formatted messages through the runtime; stage contexts associate it with the current patch.
class Logger {
  public:
    // Borrows the host's reporting table and patch identity; both must remain valid for every log call.
    explicit Logger(detail::StageAccess access = {}) noexcept : owner_(access.owner), api_(detail::reporting(access)) {}

    // Reports whether the configured severity filter admits this level.
    bool enabled(LogLevel level) const noexcept {
        return detail::table_available(api_, offsetof(FC_ReportingApi, log_enabled), sizeof(api_->log_enabled)) && api_->log_enabled &&
               api_->log_enabled(api_->context, owner_, static_cast<std::uint32_t>(level)) == 1;
    }

    // Returns true after copying text into the queue. The text and runtime record header must fit within 4 KiB.
    bool write(LogLevel level, std::string_view text) const noexcept {
        if (!detail::table_available(api_, offsetof(FC_ReportingApi, log_write), sizeof(api_->log_write)) || !api_->log_write ||
            text.size() > UINT32_MAX) {
            return false;
        }
        return api_->log_write(api_->context, owner_, static_cast<std::uint32_t>(level),
                               {text.data(), static_cast<std::uint32_t>(text.size())}) == 1;
    }

    // These severity helpers copy scalar values and text for worker formatting; false means filtered or refused.
    template <detail::LogArgument... Args> bool debug(std::format_string<Args...> format, Args &&...arguments) const noexcept {
        return formatted(LogLevel::Debug, format.get(), std::forward<Args>(arguments)...);
    }

    template <detail::LogArgument... Args> bool info(std::format_string<Args...> format, Args &&...arguments) const noexcept {
        return formatted(LogLevel::Info, format.get(), std::forward<Args>(arguments)...);
    }

    template <detail::LogArgument... Args> bool warning(std::format_string<Args...> format, Args &&...arguments) const noexcept {
        return formatted(LogLevel::Warning, format.get(), std::forward<Args>(arguments)...);
    }

    template <detail::LogArgument... Args> bool error(std::format_string<Args...> format, Args &&...arguments) const noexcept {
        return formatted(LogLevel::Error, format.get(), std::forward<Args>(arguments)...);
    }

  private:
    // Encodes the format and arguments in one bounded packet; the runtime copies it before this call returns.
    template <class... Args> bool formatted(LogLevel level, std::string_view format, Args &&...arguments) const noexcept {
        if (!enabled(level) || !detail::table_available(api_, offsetof(FC_ReportingApi, log_submit), sizeof(api_->log_submit)) ||
            !api_->log_submit) {
            return false;
        }

        std::array<std::uint8_t, 4096> bytes{};

        // Submit an oversized packet through the runtime so it counts the refused record as a loss.
        const auto refused = [&] {
            const FC_LogPacket packet{{bytes.data(), static_cast<std::uint32_t>(bytes.size())}, &detail::format_log<Args...>};
            return api_->log_submit(api_->context, owner_, static_cast<std::uint32_t>(level), &packet) == 1;
        };
        std::size_t position{};
        if (!detail::encode_log(bytes, position, format) || !(detail::encode_log(bytes, position, std::forward<Args>(arguments)) && ...)) {
            return refused();
        }

        const FC_LogPacket packet{{bytes.data(), static_cast<std::uint32_t>(position)}, &detail::format_log<Args...>};
        return api_->log_submit(api_->context, owner_, static_cast<std::uint32_t>(level), &packet) == 1;
    }

    void *owner_{};
    const FC_ReportingApi *api_{};
};

// Defines a named trace channel and the queue storage it needs.
struct TraceDefinition {
    // Names are local to this patch.
    std::string name;
    // Identifies the author's payload encoding for tools that read the trace afterward.
    std::uint32_t format_version{};
    // Requested queue budget in bytes, including each record's framing bytes.
    std::size_t buffer_bytes{};
};

// Captures counters and output state for one trace channel.
struct TraceHealth {
    // accepted means copied into the queue; written means accepted by ETW, independently of disk persistence.
    std::uint64_t accepted{}, written{};
    // Counts otherwise valid records lost to queue contention, capacity or output failures.
    std::uint64_t dropped{};
    bool output_failed{}, file_limit_reached{};
};

namespace detail {

template <class T> inline constexpr bool trace_span = false;
template <class T, std::size_t N> inline constexpr bool trace_span<std::span<T, N>> = true;
} // namespace detail

// Refers to a runtime trace channel created during Prepare; copies share its queue and health counters.
// The handle stays valid until exit, including after failed Prepare cleanup disables the channel.
class TraceChannel {
  public:
    TraceChannel() = default;

    TraceChannel(const FC_ReportingApi *api, void *channel) noexcept : api_(api), channel_(channel) {}

    // Reports whether the channel is enabled and its output remains available.
    bool enabled() const noexcept {
        return channel_ && detail::table_available(api_, offsetof(FC_ReportingApi, trace_enabled), sizeof(api_->trace_enabled)) &&
               api_->trace_enabled && api_->trace_enabled(api_->context, channel_) == 1;
    }

    // Returns true after copying the payload into the queue. Payloads above 60 KiB are refused.
    bool write(std::span<const std::byte> bytes) const noexcept {
        if (!channel_ || bytes.size() > UINT32_MAX ||
            !detail::table_available(api_, offsetof(FC_ReportingApi, trace_write), sizeof(api_->trace_write)) || !api_->trace_write) {
            return false;
        }
        return api_->trace_write(api_->context, channel_,
                                 {reinterpret_cast<const std::uint8_t *>(bytes.data()), static_cast<std::uint32_t>(bytes.size())}) == 1;
    }

    // Copies the object's raw representation, including padding and pointer values; use self-contained records.
    template <class T>
        requires(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> && !detail::trace_span<std::remove_cvref_t<T>>)
    bool write(const T &record) const noexcept {
        return write(std::as_bytes(std::span{&record, 1}));
    }

    // Copies channel counters and output flags; an empty handle or unavailable callback yields zero values.
    TraceHealth health() const noexcept {
        FC_TraceHealth result{};
        if (channel_ && detail::table_available(api_, offsetof(FC_ReportingApi, trace_health), sizeof(api_->trace_health)) &&
            api_->trace_health) {
            api_->trace_health(api_->context, channel_, &result);
        }
        return {result.accepted, result.written, result.dropped, result.output_failed != 0, result.file_limit_reached != 0};
    }

  private:
    const FC_ReportingApi *api_{};
    void *channel_{};
};

namespace detail {

// Creates an owner-bound channel during Prepare, returning the runtime's synchronous error if creation fails.
inline Result<TraceChannel> create_trace(StageAccess access, const TraceDefinition &definition) {
    const auto *api = reporting(access);
    if (!table_available(api, offsetof(FC_ReportingApi, trace), sizeof(api->trace)) || !api->trace) {
        return std::unexpected(Error{"Trace creation is unavailable"});
    }

    Error error{"The trace channel could not be created"};
    // Copy the callback's borrowed message into the SDK error before the runtime returns.
    const FC_ErrorSink sink{&error, [](void *context, FC_Text message, std::uint32_t code) {
                                auto &result = *static_cast<Error *>(context);
                                result.windows_error = code;
                                try {
                                    result.message = view(message);
                                } catch (...) {
                                }
                            }};
    const FC_TraceDefinition request{text(definition.name), definition.format_version, static_cast<std::uint64_t>(definition.buffer_bytes)};
    void *channel{};
    if (api->trace(api->context, access.owner, &request, &channel, sink) != 1 || !channel) {
        return std::unexpected(std::move(error));
    }
    return TraceChannel{api, channel};
}
} // namespace detail

// Reports a fatal error and terminates the process; aborts directly if the runtime facility is unavailable.
[[noreturn]] inline void fatal(const Error &error) noexcept {
    const auto *api = detail::fatal_reporting.load();
    if (detail::table_available(api, offsetof(FC_ReportingApi, fatal), sizeof(api->fatal)) && api->fatal &&
        error.message.size() <= UINT32_MAX) {
        api->fatal(api->context, nullptr, {error.message.data(), static_cast<std::uint32_t>(error.message.size())}, error.windows_error);
    }
    std::abort();
}
} // namespace fc
