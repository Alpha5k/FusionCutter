#pragma once

#include "protocol.hpp"
#include "../../network_pipeline/pipeline.hpp"

#include <cstdint>
#include <limits>
#include <span>

namespace fusioncutter::patches::direct_transport {

// Pins one carrier and association generation across nested native send calls.
class TransmitGroupState {
  public:
    // Starts or nests a group without changing an already pinned carrier.
    [[nodiscard]] bool begin(std::uint32_t generation, Carrier carrier) noexcept {
        if (depth_ == (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        if (depth_ == 0) {
            generation_ = generation;
            carrier_ = carrier;
        }
        ++depth_;
        return true;
    }

    // Ends one nesting level and clears the pin when the outermost call returns.
    [[nodiscard]] bool end() noexcept {
        if (depth_ == 0) {
            return false;
        }
        if (--depth_ != 0) {
            return true;
        }
        generation_ = 0;
        carrier_ = Carrier::Galaxy;
        return true;
    }

    [[nodiscard]] bool active() const noexcept {
        return depth_ != 0;
    }

    [[nodiscard]] bool belongs_to(std::uint32_t generation) const noexcept {
        return active() && generation_ == generation;
    }

    [[nodiscard]] Carrier carrier() const noexcept {
        return carrier_;
    }

  private:
    Carrier carrier_{Carrier::Galaxy};
    std::uint32_t generation_{};
    std::uint32_t depth_{};
};

// The native game hooks are identical for client and server; each role supplies these lifecycle and packet actions.
class GameTransport {
  public:
    virtual ~GameTransport() = default;

    // Pumps patch traffic and state around one native receive pass.
    virtual void before_receive() noexcept = 0;
    virtual void after_receive() noexcept = 0;
    // Selects and pins the carrier used by one native transmit path.
    virtual void on_native_transmit(int physical_primary) noexcept = 0;
    [[nodiscard]] virtual int begin_transmit_group(int physical_primary) noexcept = 0;
    virtual void end_transmit_group(int physical_primary) noexcept = 0;
    [[nodiscard]] virtual NativeTransmitResult transmit_native(int physical_primary, int group_primary,
                                                               std::span<const std::uint8_t> bytes) noexcept = 0;
    // Tracks association lifetime from the game's packet, connection, and lobby callbacks.
    virtual void on_native_intake(void* endpoint) noexcept = 0;
    virtual void on_native_disconnect(int physical_primary) noexcept = 0;
    virtual void on_native_disconnect_complete(int) noexcept {}
    virtual void on_reset(std::uint8_t mode) noexcept = 0;
    virtual void on_remote_member(const void* member_id, std::uint32_t state) noexcept = 0;
    virtual void on_local_lobby_left() noexcept = 0;
};

// Adapts a role-specific Direct Transport instance to the shared network pipeline.
[[nodiscard]] network_pipeline::TransportCallbacks make_pipeline_callbacks(GameTransport& transport) noexcept;

} // namespace fusioncutter::patches::direct_transport
