#pragma once

#include "layout.hpp"

#include <fc/plugin.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fc::core_plugin::rcon_server::aspyr {

// Extends Aspyr's authenticated RCON command path with the established `/lua` command.
class Handler {
  public:
    Handler() = default;

    Handler(const Handler &) = delete;
    Handler &operator=(const Handler &) = delete;

    // Declares the authenticated command hook, listener observer and the game locations they use.
    // This handler must outlive the installed hooks because their callbacks capture it.
    void plan(fc::Plan &plan);

  private:
    // Handles `/lua` after authentication and passes other commands to the game's original parser.
    // Returns true for handled `/lua` commands, including rejected source, or the original parser's result.
    bool handle_command(std::int32_t admin_id, const wchar_t *line, bool is_dedicated, bool from_chat) noexcept;

    // Executes Lua source and restores the previous stack height; returns zero on success.
    // Returns -1 if the game has no Lua state, or lua_dobuffer's error code if execution fails.
    int execute_lua(std::string_view code) noexcept;

    // Copies a result and its terminating null into NetAdmin's native remote response buffer.
    // response.size() must be less than Layout::kResponseCapacity.
    void write_response(std::string_view response) noexcept;

    // Checks that the game's current RCON socket is listening; a failure stops the process.
    void check_listener() const noexcept;

    fc::NativeFunction<RemoteAdminFunction> original_;
    fc::NativeFunction<LuaDoBufferFunction> lua_execute_;
    fc::NativeFunction<LuaGetTopFunction> lua_get_top_;
    fc::NativeFunction<LuaSetTopFunction> lua_set_top_;
    fc::Memory<LuaState *const> lua_state_;
    fc::Memory<char> response_;
    fc::Memory<const std::uint64_t> listener_;
};

} // namespace fc::core_plugin::rcon_server::aspyr
