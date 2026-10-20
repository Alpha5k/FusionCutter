#include "frame.hpp"

#include <fc/reporting.hpp>

#include <cassert>
#include <limits>
#include <string_view>

namespace fc::core_plugin::lua_bindings {
namespace {

// Passes an error message and its byte length to the caller's sink before returning.
void report(fc::lua::detail::ErrorSink sink, std::string_view message) noexcept {
    if (sink) {
        sink({message.data(), static_cast<std::uint32_t>(message.size())}, 0);
    }
}

// Requires a text pointer whenever the byte length is nonzero.
bool valid_text(FC_Text text) noexcept {
    return text.data || text.size == 0;
}

// Retrieves the current frame from an SDK operation; the caller must stay within that frame's lifetime.
Frame &frame(void *identity) noexcept {
    assert(identity);
    return *static_cast<Frame *>(identity);
}

} // namespace

Frame::Frame(const FcLuaNative &native, lua_State *state, std::uint64_t id, std::uint32_t arguments, int original) noexcept
    : native_(native), state_(state), id_(id), arguments_(arguments), entry_top_(native.gettop(state)), original_(original) {}

Frame::~Frame() {
    if (!keep_stack_) {
        fc_lua_restore(&native_, state_, entry_top_);
    }
}

FC_LuaInvocation Frame::invocation() noexcept {
    return {this, id_, arguments_};
}

bool Frame::valid(FC_LuaValue value) const noexcept {
    return value.frame == this && value.index >= 0 && value.index <= native_.gettop(state_);
}

bool Frame::valid(const FC_LuaValue *values, std::uint32_t count) const noexcept {
    if ((!values && count) || count > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    for (std::uint32_t i = 0; i != count; ++i) {
        if (!valid(values[i])) {
            return false;
        }
    }
    return true;
}

bool Frame::valid(const FC_LuaIterator *iterator) const noexcept {
    return iterator && iterator->frame == this && iterator->table > 0 && iterator->key > 0 && iterator->value == iterator->key + 1 &&
           valid({iterator->frame, iterator->table}) && valid({iterator->frame, iterator->value});
}

void Frame::error(FC_Text message) noexcept {
    if (pending_) {
        return;
    }

    try {
        if (!valid_text(message)) {
            pending_.emplace("Lua error message has a nonzero length but no text buffer");
        } else {
            pending_.emplace(message.data ? message.data : "", message.size);
        }
    } catch (...) {
        fc::fatal(fc::Error{"Could not copy a pending Lua error"});
    }
}

void Frame::failed(FcLuaResult result) noexcept {
    error(result.error);
    fc_lua_restore(&native_, state_, result.saved_top);
}

bool Frame::checked(FcLuaResult result, fc::lua::detail::ErrorSink sink) noexcept {
    if (result.success) {
        return true;
    }

    try {
        // Copy the message while its Lua string is still on the stack, then restore the saved depth.
        std::string message{result.error.data ? result.error.data : "", result.error.size};
        fc_lua_restore(&native_, state_, result.saved_top);
        report(sink, message);
    } catch (...) {
        fc::fatal(fc::Error{"Could not copy a Lua operation error"});
    }
    return false;
}

FC_LuaValue Frame::push(FC_LuaInput input) noexcept {
    if ((input.type == FC_LUA_STRING || input.type == FC_LUA_USERDATA) && !valid_text(input.text)) {
        error(fc::detail::text("Lua text input has a nonzero length but no buffer"));
        return {this, 0};
    }
    if (input.type == FC_LUA_USERDATA && !input.bytes.data && input.bytes.size) {
        error(fc::detail::text("Lua userdata has a nonzero size but no data buffer"));
        return {this, 0};
    }
    if (input.type == FC_LUA_NIL) {
        return {this, 0};
    }

    const auto result = fc_lua_push(&native_, state_, input);
    if (!result.success) {
        failed(result);
        return {this, 0};
    }
    return {this, result.values.first};
}

std::uint32_t Frame::read(FC_LuaValue value, std::uint32_t kind, FC_Text tag, FC_LuaReadResult *out,
                          fc::lua::detail::ErrorSink sink) noexcept {
    // Type and metatable queries have no Result return, so their errors become pending callback errors.
    const bool pending = kind == FC_LUA_READ_TYPE || kind == FC_LUA_READ_METATABLE;
    if (!out || !valid(value) || !valid_text(tag)) {
        constexpr auto message = "Lua inspection requires a value from this frame, a valid type tag and an output pointer";
        if (pending) {
            error(fc::detail::text(message));
        } else {
            report(sink, message);
        }
        return 0;
    }

    *out = {};
    const auto result = fc_lua_read(&native_, state_, value.index, kind, tag, out);
    if (result.success) {
        return 1;
    }

    if (pending) {
        failed(result);
    } else {
        (void)checked(result, sink);
    }
    return 0;
}

FC_LuaValue Frame::global_get(FC_Text name) noexcept {
    if (!valid_text(name)) {
        error(fc::detail::text("Lua global name has a nonzero length but no buffer"));
        return {this, 0};
    }

    const auto result = fc_lua_global_get(&native_, state_, name);
    if (!result.success) {
        failed(result);
        return {this, 0};
    }
    return {this, result.values.first};
}

void Frame::global_set(FC_Text name, FC_LuaValue value) noexcept {
    if (!valid_text(name) || !valid(value)) {
        error(fc::detail::text("Lua global assignment requires a valid name and a value from this frame"));
        return;
    }

    const auto result = fc_lua_global_set(&native_, state_, name, value.index);
    if (!result.success) {
        failed(result);
    }
}

FC_LuaValue Frame::get(FC_LuaValue table, FC_LuaValue key) noexcept {
    if (!valid(table) || !valid(key)) {
        error(fc::detail::text("Lua field access requires values from this frame"));
        return {this, 0};
    }

    const auto result = fc_lua_get(&native_, state_, table.index, key.index);
    if (!result.success) {
        failed(result);
        return {this, 0};
    }
    return {this, result.values.first};
}

void Frame::set(FC_LuaValue table, FC_LuaValue key, FC_LuaValue value) noexcept {
    if (!valid(table) || !valid(key) || !valid(value)) {
        error(fc::detail::text("Lua field assignment requires values from this frame"));
        return;
    }

    const auto result = fc_lua_set(&native_, state_, table.index, key.index, value.index);
    if (!result.success) {
        failed(result);
    }
}

std::uint32_t Frame::iteration_begin(FC_LuaValue table, FC_LuaIterator *out) noexcept {
    if (!out || !valid(table) || table.index == 0) {
        error(fc::detail::text("Lua iteration requires a table from this frame"));
        return 0;
    }

    *out = {this, table.index, 0, 0};
    const auto result = fc_lua_iteration_begin(&native_, state_, table.index, out);
    if (!result.success) {
        *out = {};
        failed(result);
        return 0;
    }
    return 1;
}

std::uint32_t Frame::iteration_next(FC_LuaIterator *iterator, std::uint32_t *has_entry) noexcept {
    if (!has_entry || !valid(iterator)) {
        error(fc::detail::text("Lua iteration requires an iterator from this frame and an output pointer"));
        return 0;
    }

    *has_entry = 0;
    const auto result = fc_lua_iteration_next(&native_, state_, iterator, has_entry);
    if (!result.success) {
        failed(result);
        return 0;
    }
    return 1;
}

void Frame::iteration_end(FC_LuaIterator *iterator) noexcept {
    if (!valid(iterator)) {
        return;
    }

    const auto result = fc_lua_iteration_end(&native_, state_, iterator);
    *iterator = {};
    if (!result.success) {
        failed(result);
    }
}

std::uint32_t Frame::call(FC_LuaValue function, const FC_LuaValue *arguments, std::uint32_t count, FC_LuaRange *out,
                          fc::lua::detail::ErrorSink sink) noexcept {
    if (!out || !valid(function) || !valid(arguments, count)) {
        report(sink, "Lua call requires a function and arguments from this frame");
        return 0;
    }

    *out = {};
    if (result(sink) != 1) {
        return 0;
    }
    const auto result = fc_lua_call(&native_, state_, function.index, -2, arguments, count, 0);
    if (!checked(result, sink)) {
        return 0;
    }
    *out = result.values;
    return 1;
}

std::uint32_t Frame::execute(FC_Text source, FC_LuaRange *out, fc::lua::detail::ErrorSink sink) noexcept {
    if (!out || !valid_text(source)) {
        report(sink, "Lua execution requires valid source and output storage");
        return 0;
    }

    *out = {};
    const auto result = fc_lua_execute(&native_, state_, source);
    if (!checked(result, sink)) {
        return 0;
    }
    *out = result.values;
    return 1;
}

std::uint32_t Frame::original(std::uint32_t forward_incoming, const FC_LuaValue *arguments, std::uint32_t count, FC_LuaRange *out,
                              fc::lua::detail::ErrorSink sink) noexcept {
    if (original_ < 0) {
        report(sink, "This Lua binding has no saved original function");
        return 0;
    }
    if (!out || (!forward_incoming && !valid(arguments, count))) {
        report(sink, "Original Lua call requires arguments from this frame and valid output storage");
        return 0;
    }

    *out = {};
    const auto result =
        fc_lua_call(&native_, state_, 0, original_, arguments, forward_incoming ? arguments_ : count, forward_incoming != 0);
    if (!checked(result, sink)) {
        return 0;
    }
    *out = result.values;
    return 1;
}

std::uint32_t Frame::retain_registry(FC_LuaValue function, int *out, fc::lua::detail::ErrorSink sink) noexcept {
    if (!out || !valid(function)) {
        report(sink, "Retaining a Lua function requires a value from this frame and valid output storage");
        return 0;
    }

    *out = -2;
    return checked(fc_lua_retain_registry(&native_, state_, function.index, out), sink) ? 1u : 0u;
}

std::uint32_t Frame::call_registry(int reference, const FC_LuaValue *arguments, std::uint32_t count, FC_LuaRange *out,
                                   fc::lua::detail::ErrorSink sink) noexcept {
    if (reference < 0 || !out || !valid(arguments, count)) {
        report(sink, "Retained Lua call requires a live reference and arguments from this frame");
        return 0;
    }

    *out = {};
    if (result(sink) != 1) {
        return 0;
    }
    const auto result = fc_lua_call(&native_, state_, 0, reference, arguments, count, 0);
    if (!checked(result, sink)) {
        return 0;
    }
    *out = result.values;
    return 1;
}

void Frame::return_values(const FC_LuaValue *values, std::uint32_t count) noexcept {
    if (!valid(values, count)) {
        error(fc::detail::text("Lua return values must belong to this frame"));
        return;
    }

    if (!count) {
        returns_ = {};
        return;
    }

    // Copy now so results selected inside an iterator remain valid after it moves to the next entry.
    const auto result = fc_lua_copy_values(&native_, state_, values, count);
    if (!result.success) {
        failed(result);
    } else {
        returns_ = result.values;
    }
}

FC_LuaValue Frame::closure(FcLuaClosure *record) noexcept {
    if (!record || record->native != &native_ || !record->invoke) {
        error(fc::detail::text("Lua callback record must use this frame's native functions and provide an invoke function"));
        return {this, 0};
    }

    const auto result = fc_lua_closure(&native_, state_, record);
    if (!result.success) {
        failed(result);
        return {this, 0};
    }
    return {this, result.values.first};
}

std::uint32_t Frame::identity(FC_LuaValue value, const void **out, fc::lua::detail::ErrorSink sink) noexcept {
    if (!out || !valid(value)) {
        report(sink, "Lua identity lookup requires a value from this frame and valid output storage");
        return 0;
    }

    // Qualified Lua topointer only reads identity; it cannot allocate, invoke Lua or raise an error.
    *out = value.index == 0 ? nullptr : native_.topointer(state_, value.index);
    return 1;
}

std::uint32_t Frame::result(fc::lua::detail::ErrorSink sink) const noexcept {
    if (!pending_) {
        return 1;
    }

    report(sink, *pending_);
    return 0;
}

int Frame::finish_binding() noexcept {
    if (!pending_) {
        // Lua consumes results at the top; a final selection already occupies that range.
        if (!returns_.count || returns_.first - 1 + static_cast<int>(returns_.count) == native_.gettop(state_)) {
            keep_stack_ = true;
            return static_cast<int>(returns_.count);
        }

        // Later pushes can leave the selection below other values; copy it back to the top in that case.
        const auto result = fc_lua_copy_range(&native_, state_, returns_);
        if (result.success) {
            keep_stack_ = true;
            return static_cast<int>(result.values.count);
        }
        failed(result);
    }

    // The callback has finished; discard its temporary values and leave an error message for the C trampoline.
    fc_lua_restore(&native_, state_, 0);
    const auto prepared = fc_lua_prepare_error(&native_, state_, {pending_->data(), static_cast<std::uint32_t>(pending_->size())});
    if (!prepared.success && native_.gettop(state_) == 0) {
        fc::fatal(fc::Error{"Could not prepare the final Lua callback error"});
    }
    keep_stack_ = true;
    return -1;
}

void frame_operations(fc::lua::detail::Api &api) noexcept {
    api.read = {nullptr, [](void *, FC_LuaValue value, std::uint32_t kind, FC_Text tag, FC_LuaReadResult *out,
                            fc::lua::detail::ErrorSink sink) noexcept {
                    return frame(value.frame).read(value, kind, tag, out, sink);
                }};
    api.push = {nullptr, [](void *, void *identity, FC_LuaInput value) noexcept {
                    return frame(identity).push(value);
                }};

    api.global_get = {nullptr, [](void *, void *identity, FC_Text name) noexcept {
                          return frame(identity).global_get(name);
                      }};
    api.global_set = {nullptr, [](void *, void *identity, FC_Text name, FC_LuaValue value) noexcept {
                          frame(identity).global_set(name, value);
                      }};
    api.get = {nullptr, [](void *, FC_LuaValue table, FC_LuaValue key) noexcept {
                   return frame(table.frame).get(table, key);
               }};
    api.set = {nullptr, [](void *, FC_LuaValue table, FC_LuaValue key, FC_LuaValue value) noexcept {
                   frame(table.frame).set(table, key, value);
               }};

    api.iteration_begin = {nullptr, [](void *, FC_LuaValue table, FC_LuaIterator *out) noexcept {
                               return frame(table.frame).iteration_begin(table, out);
                           }};
    api.iteration_next = {nullptr, [](void *, FC_LuaIterator *iterator, std::uint32_t *has_entry) noexcept {
                              return frame(iterator->frame).iteration_next(iterator, has_entry);
                          }};
    api.iteration_end = {nullptr, [](void *, FC_LuaIterator *iterator) noexcept {
                             if (iterator && iterator->frame) {
                                 frame(iterator->frame).iteration_end(iterator);
                             }
                         }};

    api.call = {nullptr, [](void *, FC_LuaValue function, const FC_LuaValue *arguments, std::uint32_t count, FC_LuaRange *out,
                            fc::lua::detail::ErrorSink sink) noexcept {
                    return frame(function.frame).call(function, arguments, count, out, sink);
                }};
    api.execute = {nullptr, [](void *, void *identity, FC_Text source, FC_LuaRange *out, fc::lua::detail::ErrorSink sink) noexcept {
                       return frame(identity).execute(source, out, sink);
                   }};
    api.original = {nullptr, [](void *, void *identity, std::uint32_t forward, const FC_LuaValue *arguments, std::uint32_t count,
                                FC_LuaRange *out, fc::lua::detail::ErrorSink sink) noexcept {
                        return frame(identity).original(forward, arguments, count, out, sink);
                    }};

    api.return_values = {nullptr, [](void *, void *identity, const FC_LuaValue *values, std::uint32_t count) noexcept {
                             frame(identity).return_values(values, count);
                         }};
    api.result = {nullptr, [](void *, void *identity, fc::lua::detail::ErrorSink sink) noexcept {
                      return frame(identity).result(sink);
                  }};
    api.error = {nullptr, [](void *, void *identity, FC_Text message) noexcept {
                     frame(identity).error(message);
                 }};
}

} // namespace fc::core_plugin::lua_bindings
