#include "fixture.hpp"

#include "plugins/plugin.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

using fc::lua::BindingMode;
using fc::lua::Frame;
using fc::tests::lua::Fixture;

// Calls the saved native function and forwards its results or error to Lua.
void forward_original(Frame &frame) noexcept {
    auto result = frame.original();
    if (result) {
        frame.return_values(result->values());
    } else {
        frame.error(result.error().message);
    }
}

// Owns the handler created in the consumer DLL.
struct DllHandler {
    FC_Handler value{};

    // Destroys the handler through the consumer DLL's callback.
    ~DllHandler() {
        if (value.destroy) {
            value.destroy(value.context);
        }
    }
};

// Collects the consumer's status output through its C handler interface.
std::string status(const FC_Handler &handler) {
    std::string output;
    FC_StatusWriter writer{&output, [](void *context, FC_Text text) -> std::uint32_t {
                               static_cast<std::string *>(context)->append(text.data, text.size);
                               return 1;
                           }};
    handler.status(handler.context, &writer);
    return output;
}

} // namespace

TEST_CASE("Lua bindings initialize once and wrap newly registered native functions", "[lua][bindings]") {
    Fixture fixture;
    unsigned initialized = 0;

    REQUIRE(fixture.bindings->initialize([&](Frame &frame) noexcept {
        ++initialized;
        auto metadata = frame.table();
        metadata.set("version", frame.value("test"));
        frame.global("Extension", metadata);
    }));
    REQUIRE(fixture.bindings->bind({"Extension", "Added"}, [](Frame &frame) noexcept {
        frame.return_values({frame.value("added")});
    }));
    REQUIRE(fixture.bindings->bind("Native", BindingMode::Wrap, &forward_original));
    REQUIRE(fixture.bindings->bind("Later", BindingMode::Wrap, &forward_original));
    REQUIRE(fixture.bindings->bind("Replacement", BindingMode::Replace, [](Frame &frame) noexcept {
        frame.return_values({frame.value(!frame.original())});
    }));

    fixture.script("function Native(a, b) return a, b end; function Replacement() return false end");
    fixture.start();
    const auto first_state = fixture.state;
    fixture.script(R"(
        assert(Extension.version == "test" and Extension.Added() == "added")
        local a, b = Native(7, 8)
        assert(a == 7 and b == 8 and Replacement())
        assert(Later == nil)
        Extension.version = "script edit"
    )");

    // Repeated game initialization preserves the VM and its script-owned values.
    fixture.start();
    CHECK(initialized == 1);
    CHECK(fixture.state == first_state);
    fixture.script("assert(Extension.version == 'script edit')");

    fixture.script("function Later(value) return value + 1 end; function Native(value) return value * 2 end");
    fixture.registered("Later");
    fixture.registered("Native");
    fixture.script("assert(Later(4) == 5 and Native(4) == 8)");
}

TEST_CASE("Lua references follow VM lifetimes while coroutine callbacks use their own arguments", "[lua][bindings]") {
    Fixture fixture;
    fc::lua::FunctionRef retained;
    fc::lua::StateId callback_state;
    double coroutine_argument = 0;

    REQUIRE(fixture.bindings->bind("Capture", [&](Frame &frame) noexcept {
        callback_state = frame.state();
        coroutine_argument = frame.argument(1).as_number().value_or(0);
        auto result = frame.retain(frame.argument(0));
        if (result) {
            retained = std::move(*result);
        } else {
            frame.error(result.error().message);
        }
    }));
    fixture.start();
    fixture.script(R"(
        local thread = coroutine.create(function()
            Capture(function(value) return value + 3 end, 88)
        end)
        assert(coroutine.resume(thread))
    )");
    REQUIRE(retained);
    CHECK(callback_state == fixture.state);
    CHECK(coroutine_argument == 88);

    double returned = 0;
    REQUIRE(fixture.bindings->run(fixture.state, [&](Frame &frame) noexcept {
        auto result = frame.call(retained, {frame.value(7)});
        if (result && result->size() == 1) {
            returned = (*result)[0].as_number().value_or(0);
        }
    }));
    CHECK(returned == 10);

    bool wrong_thread_accepted = true;
    std::thread wrong_thread{[&] {
        wrong_thread_accepted = fixture.bindings->run(fixture.state, [](Frame &) noexcept {}).has_value();
    }};
    wrong_thread.join();
    CHECK_FALSE(wrong_thread_accepted);

    // Reset can run on another thread; pump releases the registry entry on the owning Lua thread.
    std::thread release{[&] {
        retained.reset();
    }};
    release.join();
    CHECK_FALSE(retained);
    fixture.pump();

    fixture.script("Capture(function() return 22 end, 0)");
    REQUIRE(retained);
    const auto retired = retained.state();
    fixture.retire();
    CHECK_FALSE(retained);
    CHECK_FALSE(fixture.bindings->run(retired, [](Frame &) noexcept {}));

    // Reuse the same Lua allocation to model a replacement VM at the same native address.
    fixture.script("Capture = nil");
    fixture.start();
    CHECK(fixture.state != retired);
    bool stale_call_failed = false;
    REQUIRE(fixture.bindings->run(fixture.state, [&](Frame &frame) noexcept {
        stale_call_failed = !frame.call(retained, {});
    }));
    CHECK(stale_call_failed);
    CHECK(lua_gettop(fixture.vm.get()) == 0);
}

TEST_CASE("A separate plugin registers Lua callbacks through the published provider contract", "[lua][plugin]") {
    const std::array paths{std::filesystem::path{FC_LUA_CONSUMER}};
    auto loaded = fc::runtime::load_plugins(paths);
    REQUIRE(loaded.diagnostics.empty());
    REQUIRE(loaded.admitted.size() == 1);

    fc::runtime::PluginRecord plugin;
    plugin.admitted = loaded.admitted.front();
    fc::runtime::PatchRecord owner;
    DllHandler handler;
    // Construction order releases provider captures before destroying the DLL handler or its runtime patch record.
    fc::tests::lua::Fixture fixture;
    owner.plugin = &plugin;
    owner.definition = &plugin.admitted.definition->patches[0];
    owner.runtime = &fixture.core;
    owner.providers.push_back(&fixture.provider);
    owner.api = std::make_unique<fc::runtime::PatchApiReservations>(owner);
    owner.state = fc::runtime::PatchState::Preparing;

    const auto &factory = owner.definition->variants[0].factory;
    REQUIRE(factory.create(factory.context, &owner, &fixture.core.host(), nullptr, 0, &handler.value, {}) == 1);
    REQUIRE(handler.value.prepare(handler.value.context, {}) == 1);
    owner.state = fc::runtime::PatchState::Installed;
    fixture.start();

    fixture.script(R"(
        assert(DllInitialized)
        local value, suffix = DllConsumer.Echo("a\000b")
        assert(value == "a\000b" and suffix == "from DLL")
    )");
    CHECK(status(handler.value) == "Live Lua states: 1");

    fixture.retire();
    fixture.control();
    CHECK(status(handler.value) == "Live Lua states: 0");
}
