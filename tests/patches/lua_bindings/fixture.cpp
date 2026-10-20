#include "fixture.hpp"

#include "../../../patches/lua_bindings/service.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace fc::tests::lua {

Fixture::Fixture() : service(std::make_unique<core_plugin::lua_bindings::Service>()) {
    // Create a provider and dependent consumer so API access uses the runtime's actual lifecycle checks.
    declaration.id = {"Core", 4};
    plugin.admitted.definition = &declaration;
    provider_definition.id = {"lua_bindings", 12};
    consumer_definition.id = {"Test", 4};
    provider.plugin = consumer.plugin = &plugin;
    provider.definition = &provider_definition;
    consumer.definition = &consumer_definition;
    provider.runtime = consumer.runtime = &core;
    provider.api = std::make_unique<runtime::PatchApiReservations>(provider);
    consumer.api = std::make_unique<runtime::PatchApiReservations>(consumer);
    provider.state = consumer.state = runtime::PatchState::Preparing;
    consumer.providers.push_back(&provider);

    // Publish after configuring the native table; keep the consumer in Prepare for callback registration.
    service->configure(native_functions());
    fc::PrepareContext provider_context{{&provider, &core.host()}};
    REQUIRE(service->publish(provider_context));
    provider.state = runtime::PatchState::Installed;

    fc::PrepareContext consumer_context{{&consumer, &core.host()}};
    auto result = fc::lua::get(consumer_context);
    REQUIRE(result);
    bindings.emplace(std::move(*result));

    REQUIRE(bindings->on_state([this](fc::lua::StateId id, bool alive) noexcept {
        if (alive) {
            state = id;
        } else if (state == id) {
            state = {};
        }
    }));
}

Fixture::~Fixture() {
    // Retire the state and release captures before member destruction closes Lua or destroys their runtime owners.
    retire();
    control();
    service.reset();
}

void Fixture::start() {
    consumer.state = runtime::PatchState::Installed;
    service->setup(vm.get());
    control();
}

void Fixture::control() {
    service->control_update();
}

void Fixture::pump() {
    service->pump(vm.get());
}

void Fixture::registered(std::string_view name) {
    service->setup(vm.get(), name);
}

void Fixture::retire() {
    service->state_ending(vm.get());
}

void Fixture::script(std::string_view source) {
    const auto result = vm.execute(source);
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result);
    }
}

} // namespace fc::tests::lua
