#pragma once

#include "outcome.hpp"
#include "target.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace fusioncutter::diagnostics {

inline constexpr std::size_t kMaximumEtlPayloadSize = 96;

struct TraceHealth {
    std::uint64_t submitted;
    std::uint64_t emitted_records;
    std::uint64_t dropped;
    std::uint64_t omitted;
    std::uint64_t writer_errors;
    std::uint64_t etw_events_lost;
    std::uint64_t etw_buffers_lost;
    std::uint32_t high_water;
    std::uint32_t unexpected_thread_records;
    std::uint32_t core_migrations;
    bool file_limit_reached;
};

// Publishes one independently versioned record schema into the process diagnostics trace.
class EtlChannel {
  public:
    EtlChannel() noexcept;
    ~EtlChannel();

    EtlChannel(const EtlChannel&) = delete;
    EtlChannel& operator=(const EtlChannel&) = delete;
    EtlChannel(EtlChannel&& other) noexcept;
    EtlChannel& operator=(EtlChannel&& other) noexcept;

    [[nodiscard]] std::expected<void, OutcomeReason> prepare(const TargetContext& target, std::string_view name,
                                                             std::uint32_t schema_version,
                                                             std::uint8_t capture_mode = 0);
    void start() noexcept;
    void stop() noexcept;

    // Copies one compact record without allocating, blocking, formatting, or calling ETW.
    void submit(std::uint16_t kind, std::span<const std::byte> payload = {}, std::uint32_t context = 0,
                std::uint16_t flags = 0) noexcept;
    void omit(std::uint64_t count = 1) noexcept;

    [[nodiscard]] TraceHealth health() const noexcept;
    [[nodiscard]] std::string_view filename() const noexcept;

  private:
    std::uint8_t channel_{};
};

} // namespace fusioncutter::diagnostics
