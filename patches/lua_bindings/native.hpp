#pragma once

#include "layout.hpp"
#include "native.h"

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace fc::core_plugin::lua_bindings {

class Service;

// Resolves the game's Lua calls and forwards its lifecycle events to the provider.
// The patch handler keeps this object alive while its hooks are installed.
class NativeIntegration {
  public:
    // Sets the Lua state field offsets shared by the supported x86 builds.
    NativeIntegration() noexcept;

    // Verifies Lua helpers and state fields and observes setup, registration, teardown and updates.
    // service must remain alive while the hooks are installed.
    void plan(fc::Plan &plan, Service &service);

    // Resolves Lua function pointers and state fields; call before installing the hooks.
    fc::Result<> prepare();

    const FcLuaNative &functions() const noexcept {
        return functions_;
    }

  private:
    // Returns the main Lua state when the game's initialization flag is set; otherwise returns nullptr.
    // Call on the game thread after prepare() succeeds.
    lua_State *state() const noexcept;

    FcLuaNative functions_{};
    std::array<fc::Memory<const std::byte>, Layout::kFunctionCount> function_memory_;
    fc::Memory<lua_State *const> state_memory_;
    fc::Memory<const std::uint8_t> initialized_memory_;
    // Prepared pointers into the executable's global fields, read by the lifecycle hooks.
    lua_State *const *state_{};
    const std::uint8_t *initialized_{};
};

} // namespace fc::core_plugin::lua_bindings
