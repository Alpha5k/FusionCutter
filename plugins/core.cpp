#include "core.hpp"

#include "../patches/colored_chats/patch.hpp"
#include "../patches/rcon_server/patch.hpp"
#include "../patches/dedicated_fixes/patch.hpp"
#include "../patches/lua_bindings/patch.hpp"
#include "../patches/lua_multiplayer/patch.hpp"
#include "../patches/unlock_frame_rate/patch.hpp"

namespace fc::core_plugin {
namespace {

// Declares the built-in client, Lua and server patches and their configuration categories.
fc::Plugin build_plugin() {
    return {
        .id = "Core",
        .version = FC_VERSION_STRING,
        .categories = {{.id = "Client", .order = 100}, {.id = "Lua", .order = 200}, {.id = "Server", .order = 300}},
        .patches =
            {
                colored_chats::definition(),
                unlock_frame_rate::definition(),
                lua_bindings::definition(),
                lua_multiplayer::definition(),
                rcon_server::definition(),
                dedicated_fixes::definition(),
            },
    };
}
} // namespace

const FC_Plugin *core_plugin() {
    // Retain the declarations for the process lifetime because the runtime keeps pointers into their storage.
    static auto *storage = new fc::detail::PluginStorage(build_plugin());
    return storage->descriptor();
}
} // namespace fc::core_plugin
