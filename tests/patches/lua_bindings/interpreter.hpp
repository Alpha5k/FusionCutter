#pragma once

#include "../../../patches/lua_bindings/native.h"

#include <fc/base.hpp>

extern "C" {
#include <lua.h>
}

#include <string_view>

namespace fc::tests::lua {

// Supplies production adapters with this interpreter's real functions and state layout on either architecture.
const FcLuaNative &native_functions();

// Refuses one matching provider stack reservation on this Lua thread, then restores normal behavior.
class StackReserveFailure {
  public:
    // Skips matching reservations before refusing one; other Lua states and slot counts use real Lua behavior.
    StackReserveFailure(lua_State *state, int slots, unsigned skip = 0);
    // Restores normal provider reservations when this scope ends.
    ~StackReserveFailure();

    StackReserveFailure(const StackReserveFailure &) = delete;
    StackReserveFailure &operator=(const StackReserveFailure &) = delete;

    bool triggered() const noexcept {
        return triggered_;
    }

  private:
    friend const FcLuaNative &native_functions();
    // Forwards to Lua except for this scope's selected reservation.
    static int FC_CDECL checkstack(lua_State *state, int slots);

    lua_State *state_;
    int slots_;
    unsigned skip_;
    bool triggered_{};
};

// Owns a Lua 5.0.2 VM configured with the game's float number type.
class Interpreter {
  public:
    // Opens the libraries used by binding and message tests, then clears their returned tables.
    Interpreter();

    Interpreter(const Interpreter &) = delete;
    Interpreter &operator=(const Interpreter &) = delete;

    // Closes the VM; fixtures must notify their provider before this destroys the state.
    ~Interpreter();

    lua_State *get() const noexcept {
        return state_;
    }

    // Runs a script and restores the entry stack depth on success or an ordinary Lua error.
    Result<> execute(std::string_view source);

  private:
    lua_State *state_{};
};

} // namespace fc::tests::lua
