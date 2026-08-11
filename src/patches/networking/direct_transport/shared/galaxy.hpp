#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace fusioncutter::patches::direct_transport {

inline constexpr std::uint32_t kGalaxyReliableImmediate = 3;

// Type-erased Galaxy packet calls consumed by the narrow networking adapter.
struct GalaxyApi {
    void* context{};
    bool (*send)(void* context, std::uint64_t peer, const void* bytes, std::uint32_t length, std::uint32_t send_type,
                 std::uint8_t channel) noexcept {};
    bool (*available)(void* context, std::uint32_t* bytes, std::uint8_t channel) noexcept {};
    bool (*read)(void* context, void* bytes, std::uint32_t capacity, std::uint32_t* bytes_read, std::uint64_t* sender,
                 std::uint8_t channel) noexcept {};
    void (*pop)(void* context, std::uint8_t channel) noexcept {};
};

// Narrow access to Galaxy's packet interface for the control channel and original carrier.
class GalaxyNetworking {
  public:
    explicit GalaxyNetworking(GalaxyApi api = {}) noexcept;
    // Adapts the game's Galaxy interface and native vtable to the narrow packet API.
    [[nodiscard]] static GalaxyNetworking from_interface(void* networking) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool send_reliable_immediate(std::uint64_t peer, std::span<const std::uint8_t> bytes,
                                               std::uint8_t channel) const noexcept;
    [[nodiscard]] bool packet_available(std::uint32_t& bytes, std::uint8_t channel) const noexcept;
    [[nodiscard]] bool read_packet(std::span<std::uint8_t> buffer, std::uint32_t& bytes_read, std::uint64_t& sender,
                                   std::uint8_t channel) const noexcept;
    [[nodiscard]] bool pop_packet(std::uint8_t channel) const noexcept;

  private:
    GalaxyApi api_{};
};

} // namespace fusioncutter::patches::direct_transport
