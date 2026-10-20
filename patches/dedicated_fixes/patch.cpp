#include "patch.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>

namespace fc::core_plugin::dedicated_fixes {
namespace {

// Holds the code and password-buffer RVAs that differ between supported server builds.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t texture_read_call, password_argument, password_buffer, lobby_presence;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",
                 .texture_read_call = 0x2ba2f4,
                 .password_argument = 0x198c2c,
                 .password_buffer = 0x1a307a0,
                 .lobby_presence = 0x1d6e81},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",
                 .texture_read_call = 0x2bb384,
                 .password_argument = 0x199bdc,
                 .password_buffer = 0x1a31c40,
                 .lobby_presence = 0x1d7f31},
};

// Repairs rendering, password and lobby behavior needed by dedicated servers.
struct Handler {
    // Plans the texture cleanup, password buffer and lobby advertisement fixes for the selected build.
    void plan(fc::Plan &plan) {
        const auto sites = std::ranges::find(kProfiles, plan.profile(), &ProfileSites::profile);
        if (sites == kProfiles.end()) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        // Replace the texture read with a false result and discard its stack argument to use the game's failure cleanup.
        plan.write(
            fc::CodeLocation{sites->texture_read_call, "texture read in /norender mode", fc::exact_bytes({0xe8, 0xb7, 0x00, 0x00, 0x00})},
            fc::byte_array<0x30, 0xc0, 0x83, 0xc4, 0x04>());

        // Load the game's password buffer address instead of the temporary password on the stack.
        // Check the original six-byte LEA, then replace it with MOV and a padding NOP.
        const auto source = plan.at(sites->password_argument, "server password argument");
        plan.check(source, 6, fc::exact_bytes({0x8d, 0x85, 0x54, 0xff, 0xff, 0xff}));
        plan.write(source, std::uint8_t{0xb8});
        plan.write_pointer(plan.at(source.rva + 1, source.name), fc::address(plan.at(sites->password_buffer, "server password buffer")));
        plan.write(plan.at(source.rva + 5, source.name), std::uint8_t{0x90});

        // Advertise the dedicated lobby and count its host as an extra player so it appears in the server list.
        plan.write(fc::CodeLocation{sites->lobby_presence, "lobby visibility and player count", fc::exact_bytes({0xff, 0x74, 0x24, 0x18})},
                   fc::byte_array<0x6a, 0x01, 0x47, 0x90>());
        // Both builds place the lobby type argument 0xde bytes after the presence argument.
        plan.write(fc::CodeLocation{sites->lobby_presence + 0xde, "dedicated lobby type", fc::exact_bytes({0x6a, 0x01})},
                   fc::byte_array<0x6a, 0x02>());
    }
};
} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "DedicatedFixes",
                     .category = "Server",
                     .enabled = true,
                     .configurable = false,
                     .failure_policy = fc::FailurePolicy::Fatal,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}
} // namespace fc::core_plugin::dedicated_fixes
