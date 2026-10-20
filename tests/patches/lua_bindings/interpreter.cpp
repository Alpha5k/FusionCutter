#include "interpreter.hpp"

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
#include <ldo.h>
#include <lstate.h>
}

#include <cassert>
#include <cstddef>
#include <expected>
#include <new>
#include <string>

namespace fc::tests::lua {
namespace {
thread_local StackReserveFailure *reserve_failure{};
} // namespace

StackReserveFailure::StackReserveFailure(lua_State *state, int slots, unsigned skip) : state_(state), slots_(slots), skip_(skip) {
    assert(!reserve_failure);
    reserve_failure = this;
}

StackReserveFailure::~StackReserveFailure() {
    reserve_failure = nullptr;
}

int FC_CDECL StackReserveFailure::checkstack(lua_State *state, int slots) {
    if (reserve_failure && !reserve_failure->triggered_ && reserve_failure->state_ == state && reserve_failure->slots_ == slots) {
        if (reserve_failure->skip_) {
            --reserve_failure->skip_;
        } else {
            reserve_failure->triggered_ = true;
            return 0;
        }
    }
    return lua_checkstack(state, slots);
}

const FcLuaNative &native_functions() {
    // Use the test interpreter's structure offsets for adapter tests on both x86 and x64.
    static const FcLuaNative functions{
        .top_offset = offsetof(lua_State, top),
        .global_offset = offsetof(lua_State, l_G),
        .stack_offset = offsetof(lua_State, stack),
        .checkstack = &StackReserveFailure::checkstack,
        .gettop = &lua_gettop,
        .settop = &lua_settop,
        .pushvalue = &lua_pushvalue,
        .replace = &lua_replace,
        .type = &lua_type,
        .toboolean = &lua_toboolean,
        .tonumber = &lua_tonumber,
        .tostring = &lua_tostring,
        .strlen = &lua_strlen,
        .touserdata = &lua_touserdata,
        .topointer = &lua_topointer,
        .pushnil = &lua_pushnil,
        .pushnumber = &lua_pushnumber,
        .pushlstring = &lua_pushlstring,
        .pushcclosure = &lua_pushcclosure,
        .pushboolean = &lua_pushboolean,
        .pushlightuserdata = &lua_pushlightuserdata,
        .newtable = &lua_newtable,
        .gettable = &lua_gettable,
        .settable = &lua_settable,
        .rawget = &lua_rawget,
        .rawset = &lua_rawset,
        .rawgeti = &lua_rawgeti,
        .rawseti = &lua_rawseti,
        .next = &lua_next,
        .getmetatable = &lua_getmetatable,
        .setmetatable = &lua_setmetatable,
        .newuserdata = &lua_newuserdata,
        .pcall = &lua_pcall,
        .loadbuffer = &luaL_loadbuffer,
        .ref = &luaL_ref,
        .unref = &luaL_unref,
        .error = &lua_error,
        .protected_call = &luaD_pcall,
    };
    return functions;
}

Interpreter::Interpreter() : state_(lua_open()) {
    if (!state_) {
        throw std::bad_alloc{};
    }

    luaopen_base(state_);
    luaopen_table(state_);
    luaopen_string(state_);
    luaopen_math(state_);

    // Library opening leaves tables on the stack; each test begins with an empty stack.
    lua_settop(state_, 0);
}

Interpreter::~Interpreter() {
    lua_close(state_);
}

Result<> Interpreter::execute(std::string_view source) {
    const auto entry_top = lua_gettop(state_);
    if (luaL_loadbuffer(state_, source.data(), source.size(), "test") != 0 || lua_pcall(state_, 0, 0, 0) != 0) {
        // Copy string errors before restoring the stack; use a fixed diagnostic for other error values.
        const auto message = lua_type(state_, -1) == LUA_TSTRING ? std::string{lua_tostring(state_, -1), lua_strlen(state_, -1)}
                                                                 : std::string{"Lua raised a non-string error"};
        lua_settop(state_, entry_top);
        return std::unexpected(Error{message});
    }

    lua_settop(state_, entry_top);
    return {};
}

} // namespace fc::tests::lua
