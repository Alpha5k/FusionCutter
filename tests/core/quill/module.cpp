#include "module_api.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct FrontendOptions : quill::FrontendOptions {
    static constexpr auto queue_type = quill::QueueType::BoundedDropping;
    static constexpr std::size_t initial_queue_capacity = 1024;
};

using Frontend = quill::FrontendImpl<FrontendOptions>;
using Logger = quill::LoggerImpl<FrontendOptions>;

Logger* logger = nullptr;
std::atomic_uint64_t drop_notifications = 0;

} // namespace

extern "C" __declspec(dllexport) int __cdecl FcQuillStart(const char* log_path, bool slow_backend) noexcept {
    if (log_path == nullptr || logger != nullptr) {
        return ERROR_INVALID_PARAMETER;
    }

    try {
        quill::BackendOptions options;
        options.sleep_duration = slow_backend ? std::chrono::seconds(30) : std::chrono::microseconds(100);
        options.transit_event_buffer_initial_capacity = 256;
        options.transit_events_soft_limit = 256;
        options.transit_events_hard_limit = 1024;
        options.error_notifier = [](const std::string& message) {
            if (message.contains("Dropped ")) {
                drop_notifications.fetch_add(1, std::memory_order_relaxed);
            }
        };
        quill::Backend::start(options);

        quill::FileSinkConfig sink_config;
        sink_config.set_open_mode('w');
        auto sink = Frontend::create_or_get_sink<quill::FileSink>(log_path, sink_config, quill::FileEventNotifier{});
        logger = Frontend::create_or_get_logger("fusioncutter-quill-acceptance", std::move(sink));
        logger->set_log_level(quill::LogLevel::Info);
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

extern "C" __declspec(dllexport) void __cdecl FcQuillLog(const char* message) noexcept {
    if (logger != nullptr && message != nullptr) {
        QUILL_LOG_INFO(logger, "{}", std::string_view(message));
    }
}

extern "C" __declspec(dllexport) std::uint64_t __cdecl FcQuillSaturate(std::uint32_t message_count) noexcept {
    if (logger == nullptr) {
        return UINT64_MAX;
    }

    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < message_count; ++index) {
        QUILL_LOG_INFO(logger, "Saturation record {}", index);
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
}

extern "C" __declspec(dllexport) std::uint64_t __cdecl FcQuillStop() noexcept {
    if (logger == nullptr) {
        return 0;
    }

    try {
        quill::Backend::stop();
        logger = nullptr;
        return drop_notifications.load(std::memory_order_relaxed);
    } catch (...) {
        return 0;
    }
}
