#pragma once

#include "frame.hpp"
#include "service.hpp"

#include <string>

namespace fc::core_plugin::lua_bindings {

// Stores a consumer's initializer or binding declaration for use in each Lua VM.
struct Service::Declaration {
    // Owns the consumer's capture until its destroy function runs on the control thread.
    fc::lua::detail::FrameCallback callback{};
    // Global name followed by literal table keys; an empty path identifies an initializer.
    std::vector<std::string> path;
    std::uint32_t mode{};
    // The consumer became inactive; keep the declaration while a live closure can still refer to it.
    bool retired{};
    // Number of apply() passes and closure callbacks using this record; capture destruction waits for zero.
    std::size_t invoking{};
};

// Keeps one consumer's initializers and bindings in registration order within each sequence.
struct Service::Consumer {
    void *owner{};
    std::vector<std::unique_ptr<Declaration>> initializers;
    std::vector<std::unique_ptr<Declaration>> bindings;
};

// Connects one declaration to its installed Lua closure and saved original in one VM.
struct Service::Binding {
    Service *service{};
    Vm *vm{};
    Declaration *declaration{};
    FcLuaClosure closure{};
    // Registry reference keeps the wrapper alive so its identity cannot be reused; -2 means no wrapper was saved.
    int wrapper{-2};
    // Lua's pointer for the saved wrapper, compared with the current target during registration.
    const void *identity{};
    // Registry reference for Wrap's saved function; -2 means no original was saved.
    int original{-2};
    bool installed{};
    // Installation failed; another native registration allows a new attempt.
    bool conflicted{};
};

// Tracks a VM's Lua thread, initializers and bindings through teardown.
struct Service::Vm {
    // Lua's shared global state pointer identifies the same VM across its coroutines.
    void *identity{};
    // Stack used for run(); bound closures use the stack passed to their callback.
    lua_State *state{};
    // ID assigned by this service distinguishes VMs even when their native addresses are reused.
    std::uint64_t id{};
    // Windows thread ID recorded when the VM is first observed.
    std::uint32_t thread{};
    bool alive{true};
    // Number of service operations and closure callbacks using this VM; deletion after teardown waits for zero.
    std::size_t invoking{};
    // Initializers already attempted in this VM, including those whose callbacks failed.
    std::vector<Declaration *> initialized;
    std::vector<std::unique_ptr<Binding>> bindings;
};

// Tracks a function saved in Lua's registry and the token used to call it later.
struct Service::Reference {
    // Teardown clears the association before the VM record or native memory can be reclaimed.
    Vm *vm{};
    std::uint64_t id{};
    // Lua registry reference; -2 means released or invalidated during teardown.
    int registry{-2};
    // Reset immediately invalidates the token; pump() removes the registry entry on the Lua thread.
    bool reset{};
};

// Owns a queued callback for execution in one VM on its Lua thread.
struct Service::Deferred {
    Vm *vm{};
    fc::lua::detail::FrameCallback callback{};
    bool invoking{};
    // Finished or canceled; the control thread destroys the capture and removes its queue charge.
    bool complete{};
};

// Owns a consumer callback for VM creation and teardown notifications on the control thread.
struct Service::StateListener {
    fc::lua::detail::StateCallback callback{};
    // Send all current live VMs on the first control update; later updates report queued changes.
    bool initial{true};
};

namespace detail {

// Asks the runtime whether this consumer is Installed by submitting an empty callback under its patch identity.
template <class R, class... A> bool active(const fc::ConsumerCallback<R(A...) noexcept> &callback) noexcept {
    fc::ConsumerCallback<void() noexcept> probe{callback.owner, callback.api, {nullptr, [](void *) noexcept {}}};
    return probe.try_call().has_value();
}

// Retrieves the Lua API through the consumer's patch API so the runtime checks preparation and provider dependencies.
template <class R, class... A>
bool preparing(const fc::ConsumerCallback<R(A...) noexcept> &callback, fc::lua::detail::ErrorSink sink) noexcept {
    if (!callback.owner || !fc::detail::table_available(callback.api, offsetof(FC_PatchApi, get), sizeof(callback.api->get)) ||
        !callback.api->get) {
        return false;
    }

    fc::lua::detail::Api table{};
    const auto contract = fc::lua::detail::contract().view();
    return callback.api->get(callback.owner, fc::detail::text("core.lua_bindings"), &contract, &table, {sink.context, sink.function}) == 1;
}

// Sends a diagnostic synchronously; the receiver must copy message if it needs the text after this call.
inline void report(fc::lua::detail::ErrorSink sink, std::string_view message) noexcept {
    if (sink) {
        sink(fc::detail::text(message), 0);
    }
}

// Saves a copy of a Lua operation's error before the frame restores its stack.
struct Failure {
    std::string message;

    // Returns an error callback that copies the message here; this object must outlive the callback's use.
    fc::lua::detail::ErrorSink sink() noexcept {
        return {this, [](void *context, FC_Text text, std::uint32_t) noexcept {
                    static_cast<Failure *>(context)->message = fc::detail::view(text);
                }};
    }
};

} // namespace detail
} // namespace fc::core_plugin::lua_bindings
