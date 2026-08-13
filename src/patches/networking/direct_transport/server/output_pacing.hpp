#pragma once

#include <cstdint>
#include <span>

namespace fusioncutter::patches::direct_transport::server {

struct PacedPacketSender {
    void* context;
    void (*send)(void*, std::uint8_t, std::span<const std::uint8_t>) noexcept;
};

enum class PacingPacketAction {
    SendNow,
    Buffered,
};

// Lets one optional server patch pace complete Direct Transport output groups.
struct OutputPacingCallbacks {
    void* context;
    void (*begin_group)(void*, std::uint8_t, std::uint32_t, int) noexcept;
    PacingPacketAction (*route_packet)(void*, std::uint8_t, std::uint32_t, std::span<const std::uint8_t>,
                                       const PacedPacketSender&) noexcept;
    void (*end_group)(void*, std::uint8_t, std::uint32_t, const PacedPacketSender&) noexcept;
    void (*service)(void*, const PacedPacketSender&) noexcept;
    void (*discard)(void*, std::uint8_t, std::uint32_t) noexcept;
};

void publish_output_pacing(const OutputPacingCallbacks& callbacks) noexcept;
void clear_output_pacing(const OutputPacingCallbacks& callbacks) noexcept;

void begin_output_group(std::uint8_t slot, std::uint32_t generation, int packet_type) noexcept;
[[nodiscard]] PacingPacketAction route_output_packet(std::uint8_t slot, std::uint32_t generation,
                                                     std::span<const std::uint8_t> bytes,
                                                     const PacedPacketSender& sender) noexcept;
void end_output_group(std::uint8_t slot, std::uint32_t generation, const PacedPacketSender& sender) noexcept;
void service_output_pacing(const PacedPacketSender& sender) noexcept;
void discard_output_pacing(std::uint8_t slot, std::uint32_t generation) noexcept;

} // namespace fusioncutter::patches::direct_transport::server
