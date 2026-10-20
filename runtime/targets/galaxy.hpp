#pragma once

#include <fc/abi/plugin.h>

#include <string_view>

namespace fc::runtime {

class LobbyService;

// Reports whether this executable profile has the native Galaxy integration.
bool supports_galaxy(std::string_view game_profile) noexcept;

// Returns a factory for the Galaxy bridge, using image labels game, galaxy and peer.
// LobbyService must outlive installed hooks; the runtime pumps Lobby and delivers callbacks after handler updates.
FC_HandlerFactory galaxy_factory(LobbyService &) noexcept;

} // namespace fc::runtime
