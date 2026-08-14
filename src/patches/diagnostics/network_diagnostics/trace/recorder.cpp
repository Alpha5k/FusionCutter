#include "recorder.hpp"

namespace fusioncutter::patches::network_diagnostics::trace {

std::expected<void, OutcomeReason> Recorder::prepare(const TargetContext& target, std::uint8_t capture_mode) {
    return channel_.prepare(target, "NetworkDiagnostics", kSchemaVersion, capture_mode);
}

void Recorder::start() noexcept {
    channel_.start();
}

void Recorder::stop() noexcept {
    channel_.stop();
}

void Recorder::submit(RecordKind kind, std::span<const std::byte> payload, std::uint32_t context, std::uint16_t flags,
                      Carrier carrier) noexcept {
    const auto carrier_flags = static_cast<std::uint16_t>(static_cast<std::uint16_t>(carrier) << kCarrierShift);
    channel_.submit(static_cast<std::uint16_t>(kind), payload, context,
                    static_cast<std::uint16_t>(flags | carrier_flags));
}

void Recorder::omit(std::uint64_t count) noexcept {
    channel_.omit(count);
}

Health Recorder::health() const noexcept {
    return channel_.health();
}

std::string_view Recorder::filename() const noexcept {
    return channel_.filename();
}

} // namespace fusioncutter::patches::network_diagnostics::trace
