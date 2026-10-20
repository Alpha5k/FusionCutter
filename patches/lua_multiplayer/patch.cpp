#include "patch.hpp"

#include "service.hpp"

namespace fc::core_plugin::lua_multiplayer {
namespace {

// Owns the Lua messaging service and updates it on the runtime's control thread.
class Handler {
  public:
    // Registers the feature, script functions and callbacks during Prepare.
    fc::Result<> prepare(fc::PrepareContext &context) {
        return service_.prepare(context);
    }

    // Updates the peer list available to scripts and sends pending listener announcements.
    void update(const fc::UpdateContext &) noexcept {
        service_.update();
    }

  private:
    Service service_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "lua_multiplayer",
                     .category = "Lua",
                     .enabled = true,
                     .configurable = false,
                     .depends_on = {"core.lua_bindings"},
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B", "BF2.Steam.59EDE353"})}};
}

} // namespace fc::core_plugin::lua_multiplayer
