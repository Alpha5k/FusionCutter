#pragma once

#include "record.hpp"

#include <FusionCutter/outcome.hpp>
#include <FusionCutter/target.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

namespace fusioncutter::patches::network_diagnostics::trace {

struct Health {
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

// Owns the bounded producer rings and the background TraceLogging ETL session.
class Recorder {
  public:
    Recorder() noexcept;
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    [[nodiscard]] std::expected<void, OutcomeReason> prepare(const TargetContext& target, std::uint8_t capture_mode,
                                                             std::uint32_t maximum_file_size_mb);
    void start() noexcept;
    void stop() noexcept;

    // Copies one compact record without allocating, blocking, formatting, or calling ETW.
    void submit(RecordKind kind, std::span<const std::byte> payload = {}, std::uint32_t context = 0,
                std::uint16_t flags = RecordFlags::None, Carrier carrier = Carrier::Unknown) noexcept;
    void omit(std::uint64_t count = 1) noexcept;

    [[nodiscard]] Health health() const noexcept;
    [[nodiscard]] std::string_view filename() const noexcept;

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace fusioncutter::patches::network_diagnostics::trace
