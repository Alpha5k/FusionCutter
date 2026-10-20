#pragma once

#include "interpreter.hpp"

#include "core_runtime.hpp"
#include "patches/patch_api.hpp"

#include <fc/lua.hpp>

#include <memory>
#include <optional>
#include <string_view>

namespace fc::core_plugin::lua_bindings {
class Service;
}

namespace fc::tests::lua {

// Connects the production Lua provider and SDK consumer through real runtime patch records.
struct Fixture {
    runtime::CoreRuntime core{{}};
    // Backing declarations borrowed by the plugin and patch records below.
    FC_Plugin declaration{};
    FC_Patch provider_definition{}, consumer_definition{};
    runtime::PluginRecord plugin;
    runtime::PatchRecord provider, consumer;
    Interpreter vm;
    std::unique_ptr<core_plugin::lua_bindings::Service> service;
    std::optional<fc::lua::Bindings> bindings;
    // Latest live VM reported by the provider; cleared by that VM's teardown notification.
    fc::lua::StateId state;

    // Publishes the provider and retrieves its API while the consumer remains in Prepare.
    Fixture();

    // Retires the VM and releases provider captures while runtime owners still exist.
    ~Fixture();

    // Activates the consumer, reports this VM's setup and delivers its initial state notification.
    void start();

    // Delivers control notifications and destroys completed or canceled captures.
    void control();

    // Runs one native game update opportunity on the calling Lua thread.
    void pump();

    // Reports a native global registration after the test has installed that function.
    void registered(std::string_view name);

    // Invalidates this VM before Lua releases or reuses its storage.
    void retire();

    // Runs a Lua assertion script and includes its error in the test failure.
    void script(std::string_view source);
};

} // namespace fc::tests::lua
