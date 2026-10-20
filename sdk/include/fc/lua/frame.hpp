#pragma once

#include <fc/lua/contract.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fc::lua {

// Identifies one Lua VM lifetime, shared by its coroutines.
struct StateId {
    // Zero means absent; IDs are never reused.
    std::uint64_t value = 0;
    bool operator==(const StateId &) const = default;
};

// Selects whether a binding adds, wraps or replaces its target function.
enum class BindingMode {
    // Creates an absent target and missing parent tables; existing values remain intact.
    Add = FC_LUA_ADD,
    // Requires a function and retains it for Frame::original.
    Wrap = FC_LUA_WRAP,
    // Replaces an existing function.
    Replace = FC_LUA_REPLACE
};

// Reports whether the provider accepted deferred work; only Accepted transfers ownership.
enum class DispatchResult {
    Accepted = FC_LUA_ACCEPTED,
    Unavailable = FC_LUA_UNAVAILABLE,
    Full = FC_LUA_FULL
};

// Describes the actual Lua type without coercing its value.
enum class Type {
    Nil = FC_LUA_NIL,
    Boolean = FC_LUA_BOOLEAN,
    Number = FC_LUA_NUMBER,
    String = FC_LUA_STRING,
    Table = FC_LUA_TABLE,
    Function = FC_LUA_FUNCTION,
    LightUserdata = FC_LUA_LIGHT_USERDATA,
    Userdata = FC_LUA_USERDATA,
    Thread = FC_LUA_THREAD
};

namespace detail {

// Copies synchronous error text and captures allocation failures until control returns to C++.
struct ErrorCopy {
    Error value;
    std::exception_ptr failure;

    // Saves an owned copy before the provider releases its temporary error text.
    static void FC_CDECL write(void *context, FC_Text text, std::uint32_t code) noexcept {
        auto &copy = *static_cast<ErrorCopy *>(context);
        try {
            copy.value = {std::string{fc::detail::view(text)}, code};
        } catch (...) {
            copy.failure = std::current_exception();
        }
    }

    // Borrows this error copy for one synchronous provider call.
    ErrorSink sink() noexcept {
        return {this, &write};
    }

    // Rethrows a failed copy after the C call, or supplies an operation-specific error.
    Error take(std::string_view fallback) {
        if (failure) {
            std::rethrow_exception(failure);
        }

        if (value.message.empty()) {
            value.message = fallback;
        }

        return std::move(value);
    }

    // Rethrows a failed error copy even if the provider subsequently reported success.
    void check() const {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
};

} // namespace detail

class Frame;

// Borrows a value from the current invocation; strings and userdata bytes share that lifetime.
class Value {
  public:
    // Reports the actual type without coercion.
    Type type() const noexcept;

    // Requires a Lua boolean; other types return an error.
    Result<bool> as_bool() const;

    // Requires a Lua number; returning double preserves the game's existing float32 precision.
    Result<double> as_number() const;

    // Requires a Lua string; the borrowed counted bytes may contain NUL.
    Result<std::string_view> as_string() const;

    // Accepts light userdata only; the patch remains responsible for the pointed object's lifetime.
    Result<void *> as_pointer() const;

    // Borrows bytes only from FC userdata carrying this exact tag.
    Result<Bytes> as_userdata(std::string_view type_name) const;

    // Reports whether the value has a metatable, so serializers can reject custom behavior.
    bool has_metatable() const;

    // Reads or assigns table entries through ordinary Lua access, including metamethods; missing fields read as nil.
    // Keys and assigned values must belong to this frame; a Lua access error marks the frame as failed.
    Value get(Value key) const;

    // Assigning nil removes a stored field.
    void set(Value key, Value value);

    // Reports this value's pending frame error without clearing it; handled Result errors remain separate.
    Result<> result() const;

    // Visits stored entries in unspecified order; views expire after each visitor call.
    // Returns true for completion, false for a visitor stop, or the frame error for failed traversal or cleanup.
    template <class F> Result<bool> for_each(F function);

    // These overloads convert strings, booleans and numbers into Lua keys for get and set.
    Value get(std::string_view key) const;
    Value get(const char *key) const;
    Value get(bool key) const;
    template <class N>
        requires(std::is_arithmetic_v<N> && !std::same_as<N, bool>)
    Value get(N key) const;

    void set(std::string_view key, Value value);
    void set(const char *key, Value value);
    void set(bool key, Value value);
    template <class N>
        requires(std::is_arithmetic_v<N> && !std::same_as<N, bool>)
    void set(N key, Value value);

  private:
    friend class Frame;

    Value(Frame &frame, FC_LuaValue value) noexcept : frame_(&frame), value_(value) {}

    // Converts strictly and returns an owned copy of any error.
    Result<FC_LuaReadResult> read(std::uint32_t kind, std::string_view tag = {}) const;

    Frame *frame_;
    FC_LuaValue value_;
};

// Borrows call results kept on Lua's stack until the invocation ends.
class Results {
  public:
    std::size_t size() const noexcept {
        return values_.size();
    }

    Value operator[](std::size_t index) const {
        return values_[index];
    }

    // Borrows this object's view array; Results owns the array's storage.
    std::span<const Value> values() const noexcept {
        return values_;
    }

  private:
    friend class Frame;
    std::vector<Value> values_;
};

// Owns one registry reference; reset schedules release on the owning Lua thread.
class FunctionRef {
  public:
    FunctionRef() = default;
    FunctionRef(const FunctionRef &) = delete;
    FunctionRef &operator=(const FunctionRef &) = delete;

    // Transfers the registry reference without changing its owning VM.
    FunctionRef(FunctionRef &&other) noexcept
        : reference_(std::exchange(other.reference_, {})), alive_(other.alive_), release_(other.release_) {}

    // Releases this handle before taking the replacement reference.
    FunctionRef &operator=(FunctionRef &&other) noexcept {
        if (this != &other) {
            reset();
            reference_ = std::exchange(other.reference_, {});
            alive_ = other.alive_;
            release_ = other.release_;
        }
        return *this;
    }

    // Schedules release of this registry reference.
    ~FunctionRef() {
        reset();
    }

    StateId state() const noexcept {
        return {reference_.state};
    }

    // Reports current validity; a subsequent call still checks the reference against its live frame.
    explicit operator bool() const noexcept {
        return reference_.record && alive_ && alive_(reference_) == 1;
    }

    // Invalidates this handle immediately and schedules registry release on the owning Lua thread.
    void reset() noexcept {
        if (reference_.record) {
            release_(std::exchange(reference_, {}));
        }
    }

  private:
    friend class Frame;

    FunctionRef(FC_LuaReference reference, const detail::Api &api) noexcept
        : reference_(reference), alive_(api.reference_alive), release_(api.reference_reset) {}

    FC_LuaReference reference_{};
    fc::Function<std::uint32_t(FC_LuaReference) noexcept> alive_{};
    fc::Function<void(FC_LuaReference) noexcept> release_{};
};

namespace detail {
template <class F> struct FrameCapture;
}

// Borrows the invoking Lua stack, which keeps values and results alive until this invocation ends.
// Frames and their borrowed views must remain on the invoking Lua thread.
class Frame {
  public:
    StateId state() const noexcept {
        return {invocation_.state};
    }

    std::size_t argument_count() const noexcept {
        return invocation_.argument_count;
    }

    // Arguments are zero-based; an index beyond the supplied arguments returns nil.
    Value argument(std::size_t index) const {
        return view({invocation_.frame, index < invocation_.argument_count ? static_cast<std::int32_t>(index + 1) : 0});
    }

    // Reads or assigns one literal global name in the invoking VM.
    Value global(std::string_view name) const {
        return view(api_->global_get(invocation_.frame, fc::detail::text(name)));
    }

    // The assigned value must belong to this frame; a Lua access error marks the frame as failed.
    void global(std::string_view name, Value value) {
        api_->global_set(invocation_.frame, fc::detail::text(name), value.value_);
    }

    // Creates a table on this frame's stack; a Lua allocation failure marks the frame as failed.
    Value table() {
        return push({.type = FC_LUA_TABLE});
    }

    // Creates a boolean on this frame's stack; Lua failures mark the frame as failed.
    Value value(bool value) {
        return push({.type = FC_LUA_BOOLEAN, .boolean = value ? 1u : 0u});
    }

    // Converts to the game's actual float32 Lua number representation.
    Value value(double value) {
        return push({.type = FC_LUA_NUMBER, .number = value});
    }

    template <class N>
        requires(std::is_arithmetic_v<N> && !std::same_as<N, bool>)
    Value value(N number) {
        return value(static_cast<double>(number));
    }

    // Copies counted string bytes, including embedded NUL, into Lua-owned storage.
    Value value(std::string_view text) {
        return push({.type = FC_LUA_STRING, .text = fc::detail::text(text)});
    }

    Value value(const char *text) {
        return value(std::string_view{text});
    }

    // Returns nil without consuming a stack slot.
    Value nil() {
        return view({invocation_.frame, 0});
    }

    // Creates light userdata; the patch remains responsible for the pointed object's lifetime.
    Value pointer(void *pointer) {
        return push({.type = FC_LUA_LIGHT_USERDATA, .pointer = pointer});
    }

    // Copies bytes into Lua-owned storage with the exact supplied type tag.
    Value userdata(std::string_view tag, Bytes bytes) {
        return push({.type = FC_LUA_USERDATA,
                     .text = fc::detail::text(tag),
                     .bytes = {reinterpret_cast<const std::uint8_t *>(bytes.data()), fc::detail::count(bytes.size())}});
    }

    // Calls a function in this frame and keeps successful results on its stack until the invocation ends.
    // Arguments must belong to this frame; a pending frame error prevents the call.
    Result<Results> call(Value function, std::span<const Value> arguments) {
        if (function.value_.frame != invocation_.frame) {
            return std::unexpected(Error{"Lua function belongs to another frame"});
        }

        auto input = lower(arguments);
        return checked([&](FC_LuaRange *range, detail::ErrorSink sink) {
            return api_->call(function.value_, input.data(), fc::detail::count(input.size()), range, sink);
        });
    }

    Result<Results> call(Value function, std::initializer_list<Value> arguments) {
        return call(function, std::span{arguments});
    }

    // Calls a live reference in this VM with arguments from this frame; stale or foreign references and pending errors fail.
    Result<Results> call(const FunctionRef &function, std::span<const Value> arguments) {
        auto input = lower(arguments);
        return checked([&](FC_LuaRange *range, detail::ErrorSink sink) {
            return api_->call_reference(invocation_.frame, function.reference_, input.data(), fc::detail::count(input.size()), range, sink);
        });
    }

    Result<Results> call(const FunctionRef &function, std::initializer_list<Value> arguments) {
        return call(function, std::span{arguments});
    }

    // Executes source in the current VM and borrows its returned values.
    Result<Results> execute(std::string_view source) {
        return checked([&](FC_LuaRange *range, detail::ErrorSink sink) {
            return api_->execute(invocation_.frame, fc::detail::text(source), range, sink);
        });
    }

    // Retains a function beyond this invocation through its owning VM's registry.
    Result<FunctionRef> retain(Value function) {
        if (function.value_.frame != invocation_.frame) {
            return std::unexpected(Error{"Lua function belongs to another frame"});
        }

        detail::ErrorCopy error;
        FC_LuaReference reference{};
        if (api_->retain(function.value_, &reference, error.sink()) != 1) {
            return std::unexpected(error.take("Could not retain Lua function"));
        }

        error.check();
        return FunctionRef{reference, *api_};
    }

    // Calls a wrapper's saved function with every incoming argument; the span overload uses exactly its contents.
    // An explicitly empty span forwards no arguments. Results remain on the stack until this invocation ends.
    Result<Results> original() {
        return original_call(true, {});
    }

    Result<Results> original(std::span<const Value> arguments) {
        return original_call(false, arguments);
    }

    // Copies selected values immediately; the latest call supplies the callback's results, unless the frame fails.
    // The default is zero results.
    void return_values(std::span<const Value> values) {
        auto input = lower(values);
        api_->return_values(invocation_.frame, input.data(), fc::detail::count(input.size()));
    }

    void return_values(std::initializer_list<Value> values) {
        return return_values(std::span{values});
    }

    // Reports the first pending frame error without clearing it; handled Result errors remain separate.
    Result<> result() const {
        detail::ErrorCopy error;
        if (api_->result(invocation_.frame, error.sink()) != 1) {
            return std::unexpected(error.take("Lua frame failed"));
        }
        error.check();
        return {};
    }

    // Marks the frame as failed with a copy of the first error; a binding raises it after the callback returns normally.
    void error(std::string_view message) {
        api_->error(invocation_.frame, fc::detail::text(message));
    }

  private:
    friend class Value;
    template <class F> friend struct detail::FrameCapture;

    Frame(const detail::Api &api, FC_LuaInvocation invocation) noexcept : api_(&api), invocation_(invocation) {}

    // Associates a borrowed stack slot with this frame's API operations.
    Value view(FC_LuaValue value) const {
        return Value{const_cast<Frame &>(*this), value};
    }

    // Pushes the selected input through the provider and returns a view of its stack slot.
    Value push(FC_LuaInput input) {
        return view(api_->push(invocation_.frame, input));
    }

    // Copies frame pointers and stack indices for the synchronous call; values stay on the original frame's stack.
    static std::vector<FC_LuaValue> lower(std::span<const Value> values) {
        std::vector<FC_LuaValue> result;
        result.reserve(values.size());
        for (const auto &value : values) {
            result.push_back(value.value_);
        }
        return result;
    }

    // Calls the provider and creates Value views for the returned stack slots.
    template <class F> Result<Results> checked(F operation) {
        detail::ErrorCopy error;
        FC_LuaRange range{};
        if (operation(&range, error.sink()) != 1) {
            return std::unexpected(error.take("Lua operation failed"));
        }

        error.check();

        // Keep only stack indices in the SDK; the invocation owns the corresponding Lua values.
        Results results;
        results.values_.reserve(range.count);
        for (std::uint32_t i = 0; i < range.count; ++i) {
            results.values_.push_back(view({invocation_.frame, range.first + static_cast<std::int32_t>(i)}));
        }
        return results;
    }

    // Preserves the distinction between incoming arguments and an explicitly empty argument list.
    Result<Results> original_call(bool forward, std::span<const Value> arguments) {
        auto input = lower(arguments);
        return checked([&](FC_LuaRange *range, detail::ErrorSink sink) {
            return api_->original(invocation_.frame, forward ? 1u : 0u, input.data(), fc::detail::count(input.size()), range, sink);
        });
    }

    const detail::Api *api_;
    FC_LuaInvocation invocation_;
};

inline Type Value::type() const noexcept {
    FC_LuaReadResult result{};
    frame_->api_->read(value_, FC_LUA_READ_TYPE, {}, &result, {});
    return static_cast<Type>(result.scalar);
}

inline Result<FC_LuaReadResult> Value::read(std::uint32_t kind, std::string_view tag) const {
    detail::ErrorCopy error;
    FC_LuaReadResult result{};
    if (frame_->api_->read(value_, kind, fc::detail::text(tag), &result, error.sink()) != 1) {
        return std::unexpected(error.take("Could not convert Lua value"));
    }

    error.check();
    return result;
}

inline Result<bool> Value::as_bool() const {
    return read(FC_LUA_READ_BOOLEAN).transform([](auto v) {
        return v.scalar != 0;
    });
}

inline Result<double> Value::as_number() const {
    return read(FC_LUA_READ_NUMBER).transform([](auto v) {
        return v.number;
    });
}

inline Result<std::string_view> Value::as_string() const {
    return read(FC_LUA_READ_STRING).transform([](auto v) {
        return fc::detail::view(v.text);
    });
}

inline Result<void *> Value::as_pointer() const {
    return read(FC_LUA_READ_POINTER).transform([](auto v) {
        return v.pointer;
    });
}

inline Result<Bytes> Value::as_userdata(std::string_view tag) const {
    return read(FC_LUA_READ_USERDATA, tag).transform([](auto v) {
        return Bytes{reinterpret_cast<const std::byte *>(v.bytes.data), v.bytes.size};
    });
}

inline bool Value::has_metatable() const {
    FC_LuaReadResult result{};
    frame_->api_->read(value_, FC_LUA_READ_METATABLE, {}, &result, {});
    return result.scalar != 0;
}

inline Value Value::get(Value key) const {
    return frame_->view(frame_->api_->get(value_, key.value_));
}

inline void Value::set(Value key, Value value) {
    frame_->api_->set(value_, key.value_, value.value_);
}

inline Value Value::get(std::string_view key) const {
    return get(frame_->value(key));
}

inline Value Value::get(const char *key) const {
    return get(std::string_view{key});
}

inline Value Value::get(bool key) const {
    return get(frame_->value(key));
}

template <class N>
    requires(std::is_arithmetic_v<N> && !std::same_as<N, bool>)
Value Value::get(N key) const {
    return get(frame_->value(key));
}

inline void Value::set(std::string_view key, Value value) {
    set(frame_->value(key), value);
}

inline void Value::set(const char *key, Value value) {
    set(std::string_view{key}, value);
}

inline void Value::set(bool key, Value value) {
    set(frame_->value(key), value);
}

template <class N>
    requires(std::is_arithmetic_v<N> && !std::same_as<N, bool>)
void Value::set(N key, Value value) {
    set(frame_->value(key), value);
}

inline Result<> Value::result() const {
    return frame_->result();
}

template <class F> Result<bool> Value::for_each(F function) {
    bool completed{};
    FC_LuaIterator iterator{};
    if (frame_->api_->iteration_begin(value_, &iterator) == 1) {
        // Clears iterator slots before checking status, including on a stop or a throwing visitor.
        struct End {
            const detail::Api *api;
            FC_LuaIterator *iterator;

            ~End() {
                api->iteration_end(iterator);
            }
        } end{frame_->api_, &iterator};

        // Lua operations finish under protection before each C++ visitor uses the reusable entry slots.
        std::uint32_t found{};
        while (frame_->api_->iteration_next(&iterator, &found) == 1) {
            if (!found) {
                completed = true;
                break;
            }
            const auto key = frame_->view({iterator.frame, iterator.key});
            const auto value = frame_->view({iterator.frame, iterator.value});
            if constexpr (std::is_void_v<std::invoke_result_t<F &, Value, Value>>) {
                std::invoke(function, key, value);
            } else if (!std::invoke(function, key, value)) {
                break;
            }
        }
    }

    auto status = result();
    if (!status) {
        return std::unexpected(std::move(status.error()));
    }
    return completed;
}

} // namespace fc::lua
