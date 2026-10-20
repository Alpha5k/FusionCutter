#pragma once

#include <fc/plugin.hpp>

namespace fc::core_plugin::rcon_server {

// Declares the required server RCON patch, enabled without a configuration toggle; initialization failure is fatal.
// Selects the GOG or Classic Collection handler by game image profile.
fc::Patch definition();

} // namespace fc::core_plugin::rcon_server
