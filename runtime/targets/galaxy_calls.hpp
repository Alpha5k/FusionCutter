#pragma once

#include "diagnostic.hpp"
#include "multiplayer.hpp"

#include <fc/base.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fc {
class Plan;
}

namespace fc::reporting {
class ReportingSession;
}

namespace fc::runtime {

class LobbyService;
struct PlanImage;

// Result of checking the native queue: empty, received, discarded for size, or failed.
enum class GalaxyPacketState {
    Empty,
    Read,
    Dropped,
    Failed
};

struct GalaxyPacket {
    GalaxyPacketState state{};
    std::uint32_t size{};
    std::uint64_t sender{};
};

// Retains prepared Galaxy calls and the listener that reports lobby data completion.
// Both live as long as installed hooks.
class GalaxyCalls final {
  public:
    // Creates call and listener state in this runtime module; reporting must outlive this object.
    GalaxyCalls(LobbyService &, fc::reporting::ReportingSession &);

    // Unregisters the listener if the factory still returns the same native instance.
    ~GalaxyCalls();

    GalaxyCalls(const GalaxyCalls &) = delete;
    GalaxyCalls &operator=(const GalaxyCalls &) = delete;

    // Adds expected function bytes and vtable targets to the plan for the selected galaxy and peer images.
    void plan(fc::Plan &, std::span<const PlanImage>);

    // Checks matchmaking and completion listener availability, reporting changed native lookup failures.
    std::expected<void, Diagnostic> lobby_status() const;

    // Checks ordinary or server networking availability, reporting changed native lookup failures.
    std::expected<void, Diagnostic> networking_status(bool server) const;

    // Uses the facade's hidden output pointer to copy the local user or lobby owner ID.
    std::optional<std::uint64_t> local_user() const noexcept;
    std::optional<std::uint64_t> lobby_owner(std::uint64_t lobby) const noexcept;

    // Registers the completion listener on the current native instance; call on the control thread.
    void refresh_listener() noexcept;

    // Sends reliable ordered data on channel 63. A native false result or immediate error returns Failed.
    CarrierResult submit(bool server, std::uint64_t recipient, Bytes) const noexcept;

    // Reads one channel 63 packet, or discards it if it exceeds buffer capacity.
    GalaxyPacket receive(bool server, std::span<std::byte> buffer) const noexcept;

    // Requests lobby data when the registered listener can report completion; returns synchronous native errors.
    std::expected<void, Diagnostic> request(std::uint64_t lobby) const;

    // Copies a cached native value. LobbyService must validate the key; an empty string means Missing.
    std::expected<std::string, Diagnostic> read(std::uint64_t lobby, std::string_view key) const;

    // Writes or withdraws a native value. LobbyService must validate the key/value,
    // and the caller must recheck local ownership.
    std::expected<void, Diagnostic> write(std::uint64_t lobby, std::string_view key, std::optional<std::string_view> value) const;

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace fc::runtime
