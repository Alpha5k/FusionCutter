#include <fc/lua.hpp>
#include <fc/plugin.hpp>

#include <expected>
#include <string>

namespace {

// Exercises Lua registration and owned captures from a separately compiled plugin.
struct Consumer {
    // State callbacks update this count on the control thread, where status reads it.
    unsigned live_states{};

    // Retrieves Lua bindings and registers initialization, echo and state callbacks during Prepare.
    fc::Result<> prepare(fc::PrepareContext &context) {
        auto bindings = fc::lua::get(context);
        if (!bindings) {
            return std::unexpected(bindings.error());
        }

        auto initialized = bindings->initialize([](fc::lua::Frame &frame) noexcept {
            frame.global("DllInitialized", frame.value(true));
        });
        if (!initialized) {
            return initialized;
        }

        // The callable and its captured string belong to this DLL; provider cleanup returns here to destroy them.
        auto bound = bindings->bind({"DllConsumer", "Echo"}, [suffix = std::string{"from DLL"}](fc::lua::Frame &frame) noexcept {
            frame.return_values({frame.argument(0), frame.value(suffix)});
        });
        if (!bound) {
            return bound;
        }

        return bindings->on_state([this](fc::lua::StateId, bool alive) noexcept {
            if (alive) {
                ++live_states;
            } else {
                --live_states;
            }
        });
    }

    // Reports the count of live VMs observed by state callbacks.
    void write_status(fc::StatusContext &status) const noexcept {
        status.line("Live Lua states: {}", live_states);
    }
};

// Declares the Lua provider dependency for the independently compiled test plugin.
fc::Plugin declaration() {
    return {.id = "LuaConsumer",
            .version = "1",
            .patches = {fc::Patch{
                .id = "Echo", .enabled = true, .depends_on = {"core.lua_bindings"}, .variants = {fc::variant<Consumer>({"LuaFixture"})}}}};
}

} // namespace

FC_PLUGIN(declaration)
