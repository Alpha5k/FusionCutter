#include "output_pacing.hpp"

#include <atomic>

namespace fusioncutter::patches::direct_transport::server {
namespace {

std::atomic<const OutputPacingCallbacks*> gOutputPacing;

} // namespace

void publish_output_pacing(const OutputPacingCallbacks& callbacks) noexcept {
    gOutputPacing.store(&callbacks, std::memory_order_release);
}

void clear_output_pacing(const OutputPacingCallbacks& callbacks) noexcept {
    auto* expected = &callbacks;
    static_cast<void>(gOutputPacing.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel));
}

void begin_output_group(std::uint8_t slot, std::uint32_t generation, int packet_type) noexcept {
    if (const auto* callbacks = gOutputPacing.load(std::memory_order_acquire); callbacks != nullptr) {
        callbacks->begin_group(callbacks->context, slot, generation, packet_type);
    }
}

PacingPacketAction route_output_packet(std::uint8_t slot, std::uint32_t generation, std::span<const std::uint8_t> bytes,
                                       const PacedPacketSender& sender) noexcept {
    if (const auto* callbacks = gOutputPacing.load(std::memory_order_acquire); callbacks != nullptr) {
        return callbacks->route_packet(callbacks->context, slot, generation, bytes, sender);
    }
    return PacingPacketAction::SendNow;
}

void end_output_group(std::uint8_t slot, std::uint32_t generation, const PacedPacketSender& sender) noexcept {
    if (const auto* callbacks = gOutputPacing.load(std::memory_order_acquire); callbacks != nullptr) {
        callbacks->end_group(callbacks->context, slot, generation, sender);
    }
}

void service_output_pacing(const PacedPacketSender& sender) noexcept {
    if (const auto* callbacks = gOutputPacing.load(std::memory_order_acquire); callbacks != nullptr) {
        callbacks->service(callbacks->context, sender);
    }
}

void discard_output_pacing(std::uint8_t slot, std::uint32_t generation) noexcept {
    if (const auto* callbacks = gOutputPacing.load(std::memory_order_acquire); callbacks != nullptr) {
        callbacks->discard(callbacks->context, slot, generation);
    }
}

} // namespace fusioncutter::patches::direct_transport::server
