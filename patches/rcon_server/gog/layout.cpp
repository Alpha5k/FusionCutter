#include "layout.hpp"

#include <string_view>

namespace fc::core_plugin::rcon_server::gog {

static constexpr std::string_view kImageProfile = "BF2.GOG.59EDF52B";
static constexpr std::uint32_t kChatSetup{0x001B2F5B};

Layout make_layout() {
    constexpr auto chat_setup_bytes = fc::byte_array<0x8D, 0x85, 0x00, 0xFC, 0xFF, 0xFF, 0x68, 0x00, 0x02, 0x00, 0x00, 0x50>();
    constexpr auto chat_format_bytes = fc::byte_array<0xFF, 0x15>();
    constexpr auto chat_result_bytes = fc::byte_array<0x83, 0xC4, 0x20>();

    Layout layout{
        .command_parser =
            {
                .rva = {0x001B0030},
                .name = "remote command parser",
                .evidence = fc::masked_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0, 0,    0,    0,   0x8A,
                                              0x45, 0x0C, 0x56, 0x8B, 0xF2, 0x89, 0x74, 0x24, 0, 0x57, 0x8B, 0xF9},
                                             {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0,    0,    0,   0xFF,
                                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0xFF}),
            },
        .response =
            {
                .rva = {0x01BA39D0},
                .name = "NetAdmin::remoteResponse",
            },
        .tcp_context =
            {
                .rva = {0x01A58EBC},
                .name = "TCP command context",
            },
        .logged_in =
            {
                .rva = {0x01B9C2E2},
                .name = "NetAdmin::remoteLoggedIn",
            },
        .chat_setup =
            {
                .rva = kChatSetup,
                .name = "argument setup for chat formatting",
                .evidence = fc::exact_bytes(chat_setup_bytes),
            },
        .chat_setup_size = chat_setup_bytes.size(),
        .chat_format =
            {
                .rva = kChatSetup + 0xC,
                .name = "chat formatting call",
                .evidence = fc::exact_bytes(chat_format_bytes),
            },
        .chat_format_size = chat_format_bytes.size(),
        .snprintf =
            {
                .rva = {0x0036C248},
                .name = "_snprintf",
            },
        .chat_formatter = {},
        .chat_formatter_size = sizeof(std::uintptr_t),
        .chat_result =
            {
                .rva = kChatSetup + 0x12,
                .name = "stack cleanup after chat formatting",
                .evidence = fc::exact_bytes(chat_result_bytes),
            },
        .lua_load =
            {
                .rva = {0x0029C0C0},
                .name = "luaL_loadbuffer",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45, 0x0C}),
            },
        .lua_call =
            {
                .rva = {0x0029CF40},
                .name = "lua_pcall",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x14, 0x83, 0xEC, 0x08, 0x56, 0x57}),
            },
        .lua_get_top =
            {
                .rva = {0x0029CC30},
                .name = "lua_gettop",
                .evidence =
                    fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0x8B, 0x41, 0x08, 0x2B, 0x41, 0x0C, 0xC1, 0xF8, 0x03, 0x5D, 0xC3}),
            },
        .lua_set_top =
            {
                .rva = {0x0029D490},
                .name = "lua_settop",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x0C, 0x85, 0xD2}),
            },
        .lua_state =
            {
                .rva = {0x01A58E50},
                .name = "Lua state",
            },
        .admin_password =
            {
                .rva = {0x01A64330},
                .name = "NetAdmin::netAdminPW",
            },
        .direct_port =
            {
                .rva = {0x003E9EF4},
                .name = "NetSetup::netDirectPort",
            },
        .is_host =
            {
                .rva = {0x01A58EBD},
                .name = "NetGame::netIsHost",
            },
        .teams =
            {
                .rva = {0x003EAAA0},
                .name = "Team::sTeams",
            },
        .winning_teams =
            {
                .rva = {0x01AB1054},
                .name = "Team::sWinningTeams",
            },
    };

    // The indirect CALL operand must refer to the declared formatter import in this image.
    layout.chat_formatter = {
        .rva = kChatSetup + 0xE,
        .name = "chat formatter operand",
        .evidence = fc::points_to(fc::Location{std::string{kImageProfile}, layout.snprintf.rva, layout.snprintf.name}),
    };
    return layout;
}

} // namespace fc::core_plugin::rcon_server::gog
