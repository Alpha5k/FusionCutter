#pragma once

#include <fc/plugin.hpp>

namespace fc::core_plugin::lua_bindings {

// Declares the Lua bindings provider for GOG, Steam and Mod Tools executables.
fc::Patch definition();

} // namespace fc::core_plugin::lua_bindings
