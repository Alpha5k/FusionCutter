#pragma once

#include "protected.h"

#include <fc/lua/contract.hpp>

#include <optional>
#include <string>

namespace fc::core_plugin::lua_bindings {

// Keeps values on the current Lua stack and saves the first error raised during the callback.
class Frame {
  public:
    // Starts a frame on a live VM's Lua thread. original is a registry reference, or -2 when no original was saved.
    Frame(const FcLuaNative &native, lua_State *state, std::uint64_t id, std::uint32_t arguments, int original = -2) noexcept;

    // Restores the saved stack depth unless finish_binding left return values or an error for Lua to consume.
    ~Frame();

    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;

    FC_LuaInvocation invocation() noexcept;

    std::uint64_t state_id() const noexcept {
        return id_;
    }

    lua_State *state() const noexcept {
        return state_;
    }

    // Copies non-nil input onto the Lua stack; nil uses index zero.
    // Failure saves a pending error and returns the same index-zero nil value.
    FC_LuaValue push(FC_LuaInput input) noexcept;

    // Reads a value without conversion between Lua types. Conversion errors go to sink;
    // failed type or metatable queries become pending callback errors.
    std::uint32_t read(FC_LuaValue value, std::uint32_t kind, FC_Text tag, FC_LuaReadResult *out, fc::lua::detail::ErrorSink sink) noexcept;

    // Reads a global by name, including its metamethods; access errors become pending callback errors.
    FC_LuaValue global_get(FC_Text name) noexcept;
    // Assigns a value from this frame to a global; assignment errors become pending callback errors.
    void global_set(FC_Text name, FC_LuaValue value) noexcept;
    // Reads a table field using Lua's ordinary access rules, including metamethods.
    FC_LuaValue get(FC_LuaValue table, FC_LuaValue key) noexcept;
    // Assigns a table field; the table, key and value must belong to this frame.
    void set(FC_LuaValue table, FC_LuaValue key, FC_LuaValue value) noexcept;

    // Reserves stack slots for the iterator's current key and value.
    std::uint32_t iteration_begin(FC_LuaValue table, FC_LuaIterator *out) noexcept;
    // Visits the next stored entry, reusing the key/value slots while preserving values created by the visitor.
    std::uint32_t iteration_next(FC_LuaIterator *iterator, std::uint32_t *has_entry) noexcept;
    // Releases the current key and value and invalidates the iterator.
    void iteration_end(FC_LuaIterator *iterator) noexcept;

    // Calls a function with arguments from this frame unless an error is pending.
    // Results stay on the stack and Lua errors go to sink.
    std::uint32_t call(FC_LuaValue function, const FC_LuaValue *arguments, std::uint32_t count, FC_LuaRange *out,
                       fc::lua::detail::ErrorSink sink) noexcept;
    // Runs a Lua source chunk and returns its results through out or its error through sink.
    std::uint32_t execute(FC_Text source, FC_LuaRange *out, fc::lua::detail::ErrorSink sink) noexcept;
    // Calls the saved original, using the incoming arguments when forward_incoming is set.
    std::uint32_t original(std::uint32_t forward_incoming, const FC_LuaValue *arguments, std::uint32_t count, FC_LuaRange *out,
                           fc::lua::detail::ErrorSink sink) noexcept;

    // Stores a function in Lua's registry and returns its reference through out.
    std::uint32_t retain_registry(FC_LuaValue function, int *out, fc::lua::detail::ErrorSink sink) noexcept;
    // Calls a checked registry reference unless an error is pending; arguments must belong to this frame.
    std::uint32_t call_registry(int reference, const FC_LuaValue *arguments, std::uint32_t count, FC_LuaRange *out,
                                fc::lua::detail::ErrorSink sink) noexcept;

    // Copies the selected return values immediately; a pending callback error takes precedence over them.
    void return_values(const FC_LuaValue *values, std::uint32_t count) noexcept;
    // Saves the callback's first error message for reporting when the invocation ends.
    void error(FC_Text message) noexcept;

    // Creates a Lua function using this callback record; the provider keeps the record alive.
    FC_LuaValue closure(FcLuaClosure *record) noexcept;
    // Gets Lua's address for a value so the provider can recognize its installed functions.
    std::uint32_t identity(FC_LuaValue value, const void **out, fc::lua::detail::ErrorSink sink) noexcept;

    // Reports a pending error to sink, or returns success; the destructor restores the stack afterward.
    std::uint32_t result(fc::lua::detail::ErrorSink sink) const noexcept;

    // Leaves return values or an error message on the stack for the C trampoline, returning their count or -1.
    // Call only after the author's callback returns.
    int finish_binding() noexcept;

  private:
    // Checks that values belong to this frame and use a valid stack index, or zero for nil.
    bool valid(FC_LuaValue value) const noexcept;
    bool valid(const FC_LuaValue *values, std::uint32_t count) const noexcept;
    // Requires adjacent key/value slots and a table belonging to this frame.
    bool valid(const FC_LuaIterator *iterator) const noexcept;

    // Saves an operation's error before restoring its temporary stack values.
    void failed(FcLuaResult result) noexcept;
    // Returns success, or reports the operation's error through sink after restoring the saved stack depth.
    bool checked(FcLuaResult result, fc::lua::detail::ErrorSink sink) noexcept;

    const FcLuaNative &native_;
    lua_State *state_;
    std::uint64_t id_;
    std::uint32_t arguments_;
    // Saved Lua stack depth, relative to the current function's base, restored when this invocation ends.
    int entry_top_;
    // Registry reference for the saved original; -2 means none was saved.
    int original_;
    // Retains the first callback error, including an empty error message.
    std::optional<std::string> pending_;
    FC_LuaRange returns_{};
    // Set when finish_binding leaves values on the stack for the C trampoline to return or raise.
    bool keep_stack_{};
};

// Fills the API table with operations on the current frame; the provider fills its lifecycle and reference operations.
void frame_operations(fc::lua::detail::Api &api) noexcept;

} // namespace fc::core_plugin::lua_bindings
