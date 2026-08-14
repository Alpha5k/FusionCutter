#pragma once

#include "record.hpp"

#include <FusionCutter/diagnostics.hpp>
#include <FusionCutter/outcome.hpp>
#include <FusionCutter/target.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace fusioncutter::patches::network_diagnostics::trace {

using Health = diagnostics::TraceHealth;

// Publishes network records through the process diagnostics channel.
class Recorder {
  public:
    [[nodiscard]] std::expected<void, OutcomeReason> prepare(const TargetContext& target, std::uint8_t capture_mode);
    void start() noexcept;
    void stop() noexcept;

    // Copies one compact record without allocating, blocking, formatting, or calling ETW.
    void submit(RecordKind kind, std::span<const std::byte> payload = {}, std::uint32_t context = 0,
                std::uint16_t flags = RecordFlags::None, Carrier carrier = Carrier::Unknown) noexcept;
    void omit(std::uint64_t count = 1) noexcept;

    [[nodiscard]] Health health() const noexcept;
    [[nodiscard]] std::string_view filename() const noexcept;

  private:
    diagnostics::EtlChannel channel_;
};

} // namespace fusioncutter::patches::network_diagnostics::trace
