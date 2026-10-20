#pragma once

#include "game.hpp"
#include "service.hpp"

#include <fc/plugin.hpp>

namespace fc::core_plugin::rcon_server::gog {

// Composes the GOG game bridge and loopback RCON service through the framework lifecycle.
class Handler {
  public:
    explicit Handler(const fc::CreateContext &context) noexcept;

    Handler(const Handler &) = delete;
    Handler &operator=(const Handler &) = delete;

    // Declares native calls, memory access and chat observation for the GOG layout.
    void plan(fc::Plan &plan);

    // Prepares game data, authentication, the listener and a worker that waits for activation.
    fc::Result<> prepare(fc::PrepareContext &);

    // Starts servicing the prepared listener after native installation.
    void activate(fc::ActivateContext &) noexcept;

    // Appends the listener's current port, activity and client count to the runtime status report.
    void write_status(fc::StatusContext &status) const noexcept;

  private:
    // Declared first so the game bridge outlives the service worker that uses it.
    GameBridge game_;
    Service service_;
};

} // namespace fc::core_plugin::rcon_server::gog
