#include "layout.hpp"

#include <string_view>

namespace fc::core_plugin::rcon_server::aspyr {

static constexpr std::string_view kImageProfile = "BF2.Classic.66702CD2";
static constexpr std::uint32_t kCommandSetup{0x00262313};

Layout make_layout() {
    // These instructions prepare the parser arguments after the game has authenticated the command.
    constexpr auto command_setup_bytes =
        fc::byte_array<0x45, 0x33, 0xC9, 0xC6, 0x05, 0xE2, 0xF0, 0x75, 0x00, 0x01, 0x45, 0x33, 0xC0, 0xC6, 0x05, 0xD7, 0xF0, 0x75, 0x00,
                       0x01, 0x48, 0x8D, 0x94, 0x24, 0x50, 0x04, 0x00, 0x00, 0x41, 0x8D, 0x49, 0xFF>();

    Layout layout{
        .command_parser =
            {
                .rva = {0x0025C7E0},
                .name = "remote command parser",
                .evidence = fc::exact_bytes({0x44, 0x88, 0x44, 0x24, 0x18, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56}),
            },
        .command_setup =
            {
                .rva = kCommandSetup,
                .name = "argument setup for the authenticated command",
                .evidence = fc::exact_bytes(command_setup_bytes),
            },
        .command_setup_size = command_setup_bytes.size(),
        .command_call = {},
        .response =
            {
                .rva = {0x009C1450},
                .name = "NetAdmin::remoteResponse",
            },
        .lua_execute =
            {
                .rva = {0x003863A0},
                .name = "lua_dobuffer",
                .evidence = fc::exact_bytes({0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0x54, 0x24, 0x20}),
            },
        .lua_get_top =
            {
                .rva = {0x00383FF0},
                .name = "lua_gettop",
                .evidence = fc::exact_bytes({0x48, 0x8B, 0x41, 0x10, 0x48, 0x2B, 0x41, 0x18, 0x48, 0xC1, 0xF8, 0x04, 0xC3}),
            },
        .lua_set_top =
            {
                .rva = {0x003849D0},
                .name = "lua_settop",
                .evidence = fc::exact_bytes({0x85, 0xD2, 0x78, 0x3F, 0x4C, 0x63, 0xC2, 0x48, 0x8B, 0x51, 0x18}),
            },
        .lua_state =
            {
                .rva = {0x009AFB38},
                .name = "Lua state",
            },
        .frame = {.rva = 0x1D9040,
                  .name = "native frame dispatcher",
                  .evidence = fc::exact_bytes({0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                               0x48, 0x83, 0xEC, 0x60, 0x80, 0x3D, 0x15, 0xA3, 0x4C, 0x00, 0x00})},
        .listener = {.rva = 0x642430, .name = "native RCON listener"},
    };

    // The call immediately follows the checked setup and must still target the recognized parser.
    layout.command_call = {
        .rva = kCommandSetup + 0x20,
        .name = "authenticated command call",
        .evidence = fc::direct_call_to(fc::Location{std::string{kImageProfile}, layout.command_parser.rva, layout.command_parser.name}),
    };
    return layout;
}

} // namespace fc::core_plugin::rcon_server::aspyr
