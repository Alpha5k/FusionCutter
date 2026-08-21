#pragma once

#include <FusionCutter/PluginApi.h>

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace fc {

// CoreLogger is the lightweight, type-erased logging capability passed to focused framework components. It keeps
// components independent of the reporting backend while preserving one filter, queue, and source format.
class CoreLogger final {
  public:
    using EnabledFunction = bool (*)(const void* context, FC_LogLevel level) noexcept;
    using WriteFunction = void (*)(void* context, std::string_view scope, FC_LogLevel level,
                                   std::string_view message) noexcept;

    constexpr CoreLogger() noexcept = default;
    constexpr CoreLogger(void* context, std::string_view scope, EnabledFunction enabled, WriteFunction write) noexcept
        : context_(context), scope_(scope), enabled_(enabled), write_(write) {}

    // The level check is intentionally separate from formatting so filtered diagnostics allocate and format nothing.
    [[nodiscard]] bool enabled(FC_LogLevel level) const noexcept {
        return enabled_ != nullptr && enabled_(context_, level);
    }

    // Dynamic text is copied by the shared backend during this call; the caller retains no message ownership burden.
    void write(FC_LogLevel level, std::string_view message) const noexcept {
        if (write_ != nullptr && enabled(level) && !message.empty()) {
            write_(context_, scope_, level, message);
        }
    }

    template <class... Arguments>
    void error(std::format_string<Arguments...> pattern, Arguments&&... arguments) const noexcept {
        format_and_write(FC_LOG_ERROR, pattern, std::forward<Arguments>(arguments)...);
    }

    template <class... Arguments>
    void warning(std::format_string<Arguments...> pattern, Arguments&&... arguments) const noexcept {
        format_and_write(FC_LOG_WARNING, pattern, std::forward<Arguments>(arguments)...);
    }

    template <class... Arguments>
    void info(std::format_string<Arguments...> pattern, Arguments&&... arguments) const noexcept {
        format_and_write(FC_LOG_INFO, pattern, std::forward<Arguments>(arguments)...);
    }

    template <class... Arguments>
    void debug(std::format_string<Arguments...> pattern, Arguments&&... arguments) const noexcept {
        format_and_write(FC_LOG_DEBUG, pattern, std::forward<Arguments>(arguments)...);
    }

  private:
    // Formatting failures stay within reporting and use one allocation-free fallback in the same scope.
    template <class... Arguments>
    void format_and_write(FC_LogLevel level, std::format_string<Arguments...> pattern,
                          Arguments&&... arguments) const noexcept {
        if (!enabled(level) || write_ == nullptr) {
            return;
        }
        try {
            write_(context_, scope_, level, std::format(pattern, std::forward<Arguments>(arguments)...));
        } catch (...) {
            constexpr std::string_view fallback = "A framework diagnostic could not be formatted";
            write_(context_, scope_, FC_LOG_ERROR, fallback);
        }
    }

    void* context_{};
    // Framework call sites use process-lifetime literals; the logger never allocates merely to retain its scope.
    std::string_view scope_;
    EnabledFunction enabled_{};
    WriteFunction write_{};
};

} // namespace fc
