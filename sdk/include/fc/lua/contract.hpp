#pragma once

#include <fc/abi/lua.h>
#include <fc/patch_api.hpp>

#include <cstdint>
#include <type_traits>

// Describe each C record's identity and field layout for the patch API.
inline fc::RecordDescription fc_description(std::type_identity<FC_LuaValue>) {
    return fc::describe<FC_LuaValue>("fc.lua.Value.v1", &FC_LuaValue::frame, &FC_LuaValue::index);
}

inline fc::RecordDescription fc_description(std::type_identity<FC_LuaRange>) {
    return fc::describe<FC_LuaRange>("fc.lua.Range.v1", &FC_LuaRange::first, &FC_LuaRange::count);
}

inline fc::RecordDescription fc_description(std::type_identity<FC_LuaIterator>) {
    return fc::describe<FC_LuaIterator>("fc.lua.Iterator.v1", &FC_LuaIterator::frame, &FC_LuaIterator::table, &FC_LuaIterator::key,
                                        &FC_LuaIterator::value);
}

inline fc::RecordDescription fc_description(std::type_identity<FC_LuaReference>) {
    return fc::describe<FC_LuaReference>("fc.lua.Reference.v1", &FC_LuaReference::record, &FC_LuaReference::state);
}

inline fc::RecordDescription fc_description(std::type_identity<FC_LuaInvocation>) {
    return fc::describe<FC_LuaInvocation>("fc.lua.Invocation.v1", &FC_LuaInvocation::frame, &FC_LuaInvocation::state,
                                          &FC_LuaInvocation::argument_count);
}

inline fc::RecordDescription fc_description(std::type_identity<FC_LuaInput>) {
    return fc::describe<FC_LuaInput>("fc.lua.Input.v1", &FC_LuaInput::type, &FC_LuaInput::boolean, &FC_LuaInput::number, &FC_LuaInput::text,
                                     &FC_LuaInput::bytes, &FC_LuaInput::pointer);
}

inline fc::RecordDescription fc_description(std::type_identity<FC_LuaReadResult>) {
    return fc::describe<FC_LuaReadResult>("fc.lua.ReadResult.v1", &FC_LuaReadResult::scalar, &FC_LuaReadResult::number,
                                          &FC_LuaReadResult::text, &FC_LuaReadResult::bytes, &FC_LuaReadResult::pointer);
}

namespace fc::lua::detail {

using ErrorSink = Function<void(FC_Text, std::uint32_t) noexcept>;
using FrameInvocation = ConsumerCallback<void(FC_LuaInvocation) noexcept>;

// Transfers an owned callback to the provider on acceptance.
struct FrameCallback {
    FrameInvocation invoke;
    // Runs in the consumer DLL during cleanup on the control thread.
    Function<void() noexcept> destroy;
};

// Owns a state notification callback; the provider invokes and destroys it on the control thread.
struct StateCallback {
    ConsumerCallback<void(std::uint64_t, std::uint32_t) noexcept> invoke;
    Function<void() noexcept> destroy;
};

// Describes the invoke and destruction functions together so both belong to the same versioned contract.
inline RecordDescription fc_description(std::type_identity<FrameCallback>) {
    return describe<FrameCallback>("fc.lua.FrameCallback.v1", &FrameCallback::invoke, &FrameCallback::destroy);
}

inline RecordDescription fc_description(std::type_identity<StateCallback>) {
    return describe<StateCallback>("fc.lua.StateCallback.v1", &StateCallback::invoke, &StateCallback::destroy);
}

// Publishes the Lua provider's C-compatible operations through the patch API.
// Error sinks run synchronously; operations returning a success flag use one for success and zero for failure.
struct Api {
    // Registers an initializer during Prepare, checking the consumer's provider dependency and taking ownership on success.
    Function<std::uint32_t(FrameCallback, ErrorSink) noexcept> initialize;

    // Registers a binding under the same requirements; copies its path keys before returning.
    Function<std::uint32_t(const FC_Text *, std::uint32_t, std::uint32_t, FrameCallback, ErrorSink) noexcept> bind;

    // Registers state notifications under the same requirements.
    Function<std::uint32_t(StateCallback, ErrorSink) noexcept> on_state;

    // Borrows a callback for synchronous execution on the live VM's Lua thread.
    Function<std::uint32_t(std::uint64_t, FrameInvocation, ErrorSink) noexcept> run;

    // Queues a callback to run on the VM's Lua thread during game updates; only FC_LUA_ACCEPTED transfers ownership.
    Function<std::uint32_t(std::uint64_t, FrameCallback) noexcept> defer;

    // Reports whether the reference and its owning VM are still live.
    Function<std::uint32_t(FC_LuaReference) noexcept> reference_alive;

    // Invalidates the reference immediately and schedules registry release on the owning Lua thread.
    Function<void(FC_LuaReference) noexcept> reference_reset;

    // Inspects or strictly converts a value; type and metatable inspection failures mark the frame as failed.
    // Strict conversion failures report through the error sink.
    Function<std::uint32_t(FC_LuaValue, std::uint32_t, FC_Text, FC_LuaReadResult *, ErrorSink) noexcept> read;

    // Creates a value for this frame; nil uses index zero and other values occupy a stack slot.
    // Lua failures mark the frame as failed.
    Function<FC_LuaValue(void *, FC_LuaInput) noexcept> push;

    // Read or assign a literal global name; Lua failures mark the frame as failed.
    Function<FC_LuaValue(void *, FC_Text) noexcept> global_get;
    Function<void(void *, FC_Text, FC_LuaValue) noexcept> global_set;

    // Read or assign a table entry through ordinary Lua access, including metamethods; Lua failures mark the frame as failed.
    Function<FC_LuaValue(FC_LuaValue, FC_LuaValue) noexcept> get;
    Function<void(FC_LuaValue, FC_LuaValue, FC_LuaValue) noexcept> set;

    // Keeps two stack slots for iterating a table's stored entries; Lua failures mark the frame as failed.
    Function<std::uint32_t(FC_LuaValue, FC_LuaIterator *) noexcept> iteration_begin;

    // Reuses those slots for the next entry and writes zero or one to the entry flag.
    Function<std::uint32_t(FC_LuaIterator *, std::uint32_t *) noexcept> iteration_next;

    // Clears the entry slots and iterator record.
    Function<void(FC_LuaIterator *) noexcept> iteration_end;

    // Calls a function with arguments from the same frame unless an error is pending; returns its result stack range.
    Function<std::uint32_t(FC_LuaValue, const FC_LuaValue *, std::uint32_t, FC_LuaRange *, ErrorSink) noexcept> call;

    // Calls a live retained function in this frame's VM unless an error is pending; returns its result stack range.
    Function<std::uint32_t(void *, FC_LuaReference, const FC_LuaValue *, std::uint32_t, FC_LuaRange *, ErrorSink) noexcept> call_reference;

    // Executes source in this frame's VM and returns the stack range containing its results.
    Function<std::uint32_t(void *, FC_Text, FC_LuaRange *, ErrorSink) noexcept> execute;

    // Retains only functions in their owning VM.
    Function<std::uint32_t(FC_LuaValue, FC_LuaReference *, ErrorSink) noexcept> retain;

    // Calls a wrapper's saved function; a nonzero forward flag passes all incoming arguments, zero uses the explicit list.
    Function<std::uint32_t(void *, std::uint32_t, const FC_LuaValue *, std::uint32_t, FC_LuaRange *, ErrorSink) noexcept> original;

    // Copies values immediately; the latest selection supplies the callback's results unless the frame fails.
    Function<void(void *, const FC_LuaValue *, std::uint32_t) noexcept> return_values;

    // Reports the first pending frame error without clearing it; handled conversion and call errors remain separate.
    Function<std::uint32_t(void *, ErrorSink) noexcept> result;

    // Marks the frame as failed with a copy of the first error; a binding raises it after the C++ callback returns.
    Function<void(void *, FC_Text) noexcept> error;
};

// Version 2 adds pending frame status; the runtime rejects other versions before copying their API tables.
inline const ApiContract<Api> &contract() {
    static const auto value =
        api<Api>("fc.lua_bindings.v2", &Api::initialize, &Api::bind, &Api::on_state, &Api::run, &Api::defer, &Api::reference_alive,
                 &Api::reference_reset, &Api::read, &Api::push, &Api::global_get, &Api::global_set, &Api::get, &Api::set,
                 &Api::iteration_begin, &Api::iteration_next, &Api::iteration_end, &Api::call, &Api::call_reference, &Api::execute,
                 &Api::retain, &Api::original, &Api::return_values, &Api::result, &Api::error);
    return value;
}

} // namespace fc::lua::detail
