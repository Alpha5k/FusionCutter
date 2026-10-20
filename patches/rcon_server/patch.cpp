#include "patch.hpp"
#include "aspyr/handler.hpp"
#include "gog/handler.hpp"

namespace fc::core_plugin::rcon_server {

fc::Patch definition() {
    return fc::Patch{.id = "RconServer",
                     .category = "Server",
                     .enabled = true,
                     .configurable = false,
                     .failure_policy = fc::FailurePolicy::Fatal,
                     .variants = {fc::variant<gog::Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server),
                                  fc::variant<aspyr::Handler>({"BF2.Classic.66702CD2"}, fc::Role::Server)}};
}
} // namespace fc::core_plugin::rcon_server
