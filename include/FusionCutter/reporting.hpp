#pragma once

#include "outcome.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace fusioncutter {

enum class LogLevel {
    Off,
    Error,
    Warning,
    Info,
    Debug,
};

namespace logging {

[[nodiscard]] bool enabled(LogLevel level) noexcept;

void write(LogLevel level, PatchId source, std::string_view message, std::string_view operation = {},
           PatchId related_patch = {}) noexcept;

inline void error(PatchId source, std::string_view message, std::string_view operation = {},
                  PatchId related_patch = {}) noexcept {
    write(LogLevel::Error, source, message, operation, related_patch);
}

inline void warning(PatchId source, std::string_view message, std::string_view operation = {},
                    PatchId related_patch = {}) noexcept {
    write(LogLevel::Warning, source, message, operation, related_patch);
}

inline void info(PatchId source, std::string_view message, std::string_view operation = {},
                 PatchId related_patch = {}) noexcept {
    write(LogLevel::Info, source, message, operation, related_patch);
}

inline void debug(PatchId source, std::string_view message, std::string_view operation = {},
                  PatchId related_patch = {}) noexcept {
    write(LogLevel::Debug, source, message, operation, related_patch);
}

} // namespace logging

namespace reporting {
class Session;
class StatusPublisher;
} // namespace reporting

class StatusSection {
  public:
    bool add(std::string_view label, std::string_view value) noexcept;

  private:
    StatusSection() = default;

    static constexpr std::size_t kFieldCapacity = 12;
    static constexpr std::size_t kLabelCapacity = 48;
    static constexpr std::size_t kValueCapacity = 192;

    struct Field {
        std::array<char, kLabelCapacity> label{};
        std::array<char, kValueCapacity> value{};
        std::size_t label_size{};
        std::size_t value_size{};
    };

    std::array<Field, kFieldCapacity> fields_{};
    std::size_t size_{};

    friend class reporting::Session;
    friend class reporting::StatusPublisher;
};

class StatusContributor {
  public:
    virtual ~StatusContributor() = default;
    virtual void write_status(StatusSection& output) const noexcept = 0;
};

} // namespace fusioncutter
