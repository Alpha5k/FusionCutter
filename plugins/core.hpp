#pragma once

#include <fc/plugin.hpp>

namespace fc::core_plugin {

// Returns the built-in plugin declarations, retained for the lifetime of the process.
const FC_Plugin *core_plugin();
} // namespace fc::core_plugin
