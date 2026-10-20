#pragma once

#include <fc/abi/plugin.h>

#include <string_view>

namespace fc::runtime {

class GameContext;

// Reports whether this executable profile has a native Game observer; Mod Tools supports Local and None.
bool supports_game_context(std::string_view profile) noexcept;

// Returns a factory for the Game observer. The context and owning record must remain alive while installed hooks can run.
FC_HandlerFactory game_context_factory(GameContext &context) noexcept;

} // namespace fc::runtime
