#include "handler.hpp"

#include "../bounded_text.hpp"
#include "../native_result.hpp"
#include "layout.hpp"

#include <fc/reporting.hpp>
#include <WinSock2.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace fc::core_plugin::rcon_server::aspyr {
namespace {

constexpr std::wstring_view kLuaPrefix = L"/lua ";
constexpr std::string_view kInvalidParameters = "invalid parameters\n";
constexpr std::string_view kOkay = "ok\n";
constexpr std::string_view kLuaError = "lua error\n";

} // namespace

void Handler::plan(fc::Plan &plan) {
    const auto layout = make_layout();

    // Check the instructions that set up the authenticated command and the parser they call.
    plan.check(layout.command_setup, layout.command_setup_size);
    plan.function(layout.command_parser);

    lua_execute_ = plan.function(layout.lua_execute);
    lua_get_top_ = plan.function(layout.lua_get_top);
    lua_set_top_ = plan.function(layout.lua_set_top);
    lua_state_ = plan.access(layout.lua_state);
    response_ = plan.access(layout.response);
    listener_ = plan.access(layout.listener);

    // Intercept the authenticated call while retaining the original parser for other commands.
    original_ = plan.hook_call(layout.command_call, [this](std::int32_t id, const wchar_t *line, bool dedicated, bool chat) noexcept {
        return handle_command(id, line, dedicated, chat);
    });

    // Check the current listener at each frame entry, including any replacement made during the previous frame.
    plan.observe(layout.frame, fc::before([this](void *) noexcept {
                     check_listener();
                 }));
}

void Handler::check_listener() const noexcept {
    // Preserve the game's Winsock error while checking that its current socket is listening.
    const auto saved_error = WSAGetLastError();
    auto socket = listener_.read();
    if (!socket) {
        fc::fatal(socket.error());
    }

    const auto handle = static_cast<SOCKET>(*socket);
    int accepting{}, length = sizeof(accepting);
    const auto result = getsockopt(handle, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char *>(&accepting), &length);
    const auto error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    WSASetLastError(saved_error);
    if (result == SOCKET_ERROR || !accepting) {
        fc::fatal(fc::Error{"Classic RCON listener is unavailable", static_cast<std::uint32_t>(error)});
    }
}

bool Handler::handle_command(std::int32_t admin_id, const wchar_t *line, bool is_dedicated, bool from_chat) noexcept {
    const auto pass_through = [&]() noexcept {
        return native_result(original_.call(admin_id, line, is_dedicated, from_chat));
    };
    if (line == nullptr) {
        return pass_through();
    }

    const auto input = bounded_text(line, Layout::kCommandCapacity);
    if (!input.starts_with(kLuaPrefix)) {
        return pass_through();
    }

    // Native RCON consumes the response after the authenticated parser call returns.
    // Complete Lua execution and response writing synchronously before returning.
    const auto body = input.substr(kLuaPrefix.size());
    if (body.empty() || input.size() == Layout::kCommandCapacity) {
        write_response(kInvalidParameters);
        return true;
    }

    // Accept ASCII Lua source and copy it into the byte buffer expected by lua_dobuffer.
    std::array<char, Layout::kCommandCapacity> code{};
    for (std::size_t index = 0; index < body.size(); ++index) {
        if (body[index] > 0x7f) {
            write_response(kInvalidParameters);
            return true;
        }
        code[index] = static_cast<char>(body[index]);
    }

    const auto result = execute_lua({code.data(), body.size()});
    write_response(result == 0 ? kOkay : kLuaError);
    return true;
}

int Handler::execute_lua(std::string_view code) noexcept {
    const auto state = native_result(lua_state_.read());
    if (!state) {
        return -1;
    }

    // Restore the Lua stack after execution, including any returned values or error left by lua_dobuffer.
    const auto top = native_result(lua_get_top_.call(state));
    const auto result = native_result(lua_execute_.call(state, code.data(), code.size()));
    native_result(lua_set_top_.call(state, top));
    return result;
}

void Handler::write_response(std::string_view response) noexcept {
    auto buffer = native_result(response_.view());
    std::memcpy(buffer.data(), response.data(), response.size());
    buffer[response.size()] = '\0';
}

} // namespace fc::core_plugin::rcon_server::aspyr
