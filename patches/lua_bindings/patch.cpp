#include "patch.hpp"

#include "native.hpp"
#include "service.hpp"

namespace fc::core_plugin::lua_bindings {
namespace {

// Prepares the game's Lua calls and publishes the provider as part of the patch lifecycle.
class Handler {
  public:
    void plan(fc::Plan &plan) {
        native_.plan(plan, service_);
    }

    // Resolves Lua calls and supplies them to the service before publishing its API.
    fc::Result<> prepare(fc::PrepareContext &context) {
        if (auto result = native_.prepare(); !result) {
            return result;
        }

        service_.configure(native_.functions());
        return service_.publish(context);
    }

    // Delivers state notifications and releases retired captures on the control thread.
    void update(const fc::UpdateContext &) noexcept {
        service_.control_update();
    }

  private:
    NativeIntegration native_;
    Service service_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "lua_bindings",
                     .category = "Lua",
                     .enabled = true,
                     .configurable = false,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B", "BF2.Steam.59EDE353", "BF2.ModTools.43EBD102"})}};
}

} // namespace fc::core_plugin::lua_bindings
