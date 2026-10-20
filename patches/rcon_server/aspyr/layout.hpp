#pragma once

#include <fc/plugin.hpp>

#include <cstddef>
#include <cstdint>

namespace fc::core_plugin::rcon_server::aspyr {

// Signature of the game's remote command parser, also used by the replacement callback.
using RemoteAdminFunction = bool (*)(std::int32_t admin_id, const wchar_t *line, bool is_dedicated, bool from_chat) noexcept;

struct LuaState;
using LuaDoBufferFunction = int (*)(LuaState *state, const char *code, std::size_t size) noexcept;
using LuaGetTopFunction = int (*)(LuaState *state) noexcept;
using LuaSetTopFunction = void (*)(LuaState *state, int index) noexcept;
using FrameFunction = bool (*)(void *frame) noexcept;

// Identifies the Classic Collection BF2 functions and buffers used by the RCON extension.
struct Layout {
    // Capacities include the terminating null and count char elements for responses and wchar_t elements for commands.
    static constexpr std::size_t kResponseCapacity = 0x1800;
    static constexpr std::size_t kCommandCapacity = 0x400;

    fc::FunctionLocation<RemoteAdminFunction> command_parser;
    // Instructions that prepare the parser arguments after TCP command authentication.
    fc::CodeLocation command_setup;
    // Number of instruction bytes checked at command_setup.
    std::size_t command_setup_size;
    // Parser call immediately following command_setup; this is the hook location.
    fc::CallLocation<RemoteAdminFunction> command_call;
    fc::DataLocation<char, kResponseCapacity> response;

    // lua_dobuffer leaves returned values or errors on the Lua stack.
    fc::FunctionLocation<LuaDoBufferFunction> lua_execute;
    fc::FunctionLocation<LuaGetTopFunction> lua_get_top;
    fc::FunctionLocation<LuaSetTopFunction> lua_set_top;
    fc::DataLocation<LuaState *const> lua_state;

    // Called after native startup; the hook checks the listener at frame entry.
    fc::FunctionLocation<FrameFunction> frame;
    // The game's current RCON socket handle, which can change between frames.
    fc::DataLocation<const std::uint64_t> listener;
};

// Constructs the layout for BF2.Classic.66702CD2.
Layout make_layout();

} // namespace fc::core_plugin::rcon_server::aspyr
