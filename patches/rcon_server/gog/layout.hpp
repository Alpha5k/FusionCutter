#pragma once

#include <fc/plugin.hpp>

#include <cstddef>
#include <cstdint>

namespace fc::core_plugin::rcon_server::gog {

// Native function types preserve the game's calling conventions; formatter sizes count char elements.
using FormatterFunction = int(__cdecl *)(char *buffer, std::size_t size, const char *format, ...);

struct LuaState;
// Lua buffer sizes count code bytes, excluding any terminating null.
using LuaLoadBufferFunction = int(__cdecl *)(LuaState *state, const char *code, std::size_t size, const char *name) noexcept;
using LuaProtectedCallFunction = int(__cdecl *)(LuaState *state, int arguments, int results, int error_function) noexcept;
using LuaGetTopFunction = int(__cdecl *)(LuaState *state) noexcept;
using LuaSetTopFunction = void(__cdecl *)(LuaState *state, int index) noexcept;

// The game passes two arguments in registers and leaves both boolean stack slots for caller cleanup.
using RemoteAdminCall = fc::NativeCall<bool(std::int32_t admin_id, const wchar_t *line, bool is_dedicated, bool from_chat) noexcept,
                                       fc::abi::Ecx, fc::abi::Edx, fc::abi::CallerCleanup>;

// Team::sTeams points to the game's array of opaque team records.
struct Team;
using TeamTable = Team *const *;

// Groups GOG image locations for command execution, Lua calls and chat capture.
struct Layout {
    // Response and password capacities count char elements, including space for the terminating null.
    // Command capacity counts wchar_t elements, including space for the terminating null.
    static constexpr std::size_t kResponseCapacity = 0x1800;
    static constexpr std::size_t kPasswordCapacity = 0x10;
    static constexpr std::size_t kCommandCapacity = 0x100;
    static constexpr std::uint32_t kNoWinner = 0;

    fc::FunctionLocation<RemoteAdminCall> command_parser;
    fc::DataLocation<char, kResponseCapacity> response;
    fc::DataLocation<std::uint8_t> tcp_context;
    fc::DataLocation<std::uint8_t> logged_in;

    // Instructions that prepare the formatter's buffer and size arguments; chat_setup_size counts their bytes.
    fc::CodeLocation chat_setup;
    std::size_t chat_setup_size;
    // Indirect call instruction and the number of opcode bytes checked before its operand.
    fc::CodeLocation chat_format;
    std::size_t chat_format_size;
    // Imported formatter pointer and the call operand expected to refer to it.
    // chat_formatter_size counts the operand's bytes.
    fc::DataLocation<FormatterFunction> snprintf;
    fc::CodeLocation chat_formatter;
    std::size_t chat_formatter_size;
    // Stack cleanup instruction observed after the formatter has filled the chat buffer.
    fc::CodeLocation chat_result;

    fc::FunctionLocation<LuaLoadBufferFunction> lua_load;
    fc::FunctionLocation<LuaProtectedCallFunction> lua_call;
    fc::FunctionLocation<LuaGetTopFunction> lua_get_top;
    fc::FunctionLocation<LuaSetTopFunction> lua_set_top;
    fc::DataLocation<LuaState *const> lua_state;

    fc::DataLocation<const char, kPasswordCapacity> admin_password;
    fc::DataLocation<const std::uint16_t> direct_port;
    fc::DataLocation<const std::uint8_t> is_host;
    fc::DataLocation<const TeamTable> teams;
    fc::DataLocation<const std::uint32_t> winning_teams;
};

// Pairs locations in the reviewed GOG image with their native types and validation evidence.
Layout make_layout();

} // namespace fc::core_plugin::rcon_server::gog
