#pragma once

#include <fc/contexts.hpp>
#include <fc/lua/frame.hpp>
#include <fc/reporting.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace fc::lua::detail {

// Keeps the callable and its API table in the consumer DLL; run borrows this capture synchronously.
// Accepted registrations and deferred work retain it until cleanup on the control thread.
template <class F> struct FrameCapture {
    Api api;
    F function;

    // Borrows the supplied Lua invocation and reports an unexpected C++ exception as fatal.
    static void FC_CDECL invoke(void *context, FC_LuaInvocation invocation) noexcept {
        auto &capture = *static_cast<FrameCapture *>(context);
        Frame frame{capture.api, invocation};
        try {
            std::invoke(capture.function, frame);
        } catch (...) {
            fatal(Error{"Lua callback threw a C++ exception"});
        }
    }

    // Deletes the callable in its allocating DLL during cleanup on the control thread.
    static void FC_CDECL destroy(void *context) noexcept {
        delete static_cast<FrameCapture *>(context);
    }

    // Supplies the callback's consumer identity and destruction function with this capture.
    FrameCallback callback(void *owner, const FC_PatchApi *service) noexcept {
        return {{owner, service, {this, &invoke}}, {this, &destroy}};
    }
};

// Owns one state listener in the consumer DLL.
template <class F> struct StateCapture {
    F function;

    // Delivers the VM's lifetime ID and live flag; unexpected C++ exceptions are fatal.
    static void FC_CDECL invoke(void *context, std::uint64_t state, std::uint32_t alive) noexcept {
        auto &capture = *static_cast<StateCapture *>(context);
        try {
            std::invoke(capture.function, StateId{state}, alive != 0);
        } catch (...) {
            fatal(Error{"Lua state callback threw a C++ exception"});
        }
    }

    // Deletes the listener in its allocating DLL during cleanup on the control thread.
    static void FC_CDECL destroy(void *context) noexcept {
        delete static_cast<StateCapture *>(context);
    }

    // Supplies the listener's consumer identity and destruction function with this capture.
    StateCallback callback(void *owner, const FC_PatchApi *service) noexcept {
        return {{owner, service, {this, &invoke}}, {this, &destroy}};
    }
};

} // namespace fc::lua::detail

namespace fc::lua {

// Retains the provider's API table and consumer identity; declarations register during Prepare.
// Frame callbacks run on the owning Lua thread; state notifications and capture cleanup run on the control thread.
class Bindings {
  public:
    Bindings() = default;

    // Registers F(Frame&) during Prepare to run once per VM before this patch's bindings install.
    // For an existing VM, it runs on that VM's Lua thread after activation, during setup, native registration or a game update.
    template <class F> Result<> initialize(F function) {
        if (!api_.initialize) {
            return std::unexpected(Error{"Lua bindings are unavailable"});
        }

        auto capture = std::make_unique<detail::FrameCapture<F>>(api_, std::move(function));
        detail::ErrorCopy error;
        if (api_.initialize(capture->callback(owner_, service_), error.sink()) != 1) {
            return std::unexpected(error.take("Could not register Lua initializer"));
        }

        // Transfer capture ownership before check() can rethrow a failure to copy the provider's error.
        capture.release();
        error.check();
        return {};
    }

    // Registers F(Frame&) during Prepare at a literal global name or table-key path; omitted mode defaults to Add.
    // A dot in a single name remains part of that name.
    template <class F> Result<> bind(std::string_view name, F function) {
        return bind({name}, BindingMode::Add, std::move(function));
    }

    template <class F> Result<> bind(std::string_view name, BindingMode mode, F function) {
        return bind({name}, mode, std::move(function));
    }

    template <class F> Result<> bind(std::initializer_list<std::string_view> path, F function) {
        return bind(path, BindingMode::Add, std::move(function));
    }

    // Treats each path element as one literal table key; the provider copies the keys before returning.
    // Accepted declarations apply to current and future VMs and transfer callback ownership to the provider.
    template <class F> Result<> bind(std::initializer_list<std::string_view> path, BindingMode mode, F function) {
        if (!api_.bind) {
            return std::unexpected(Error{"Lua bindings are unavailable"});
        }

        std::vector<FC_Text> keys;
        keys.reserve(path.size());
        for (auto key : path) {
            keys.push_back(fc::detail::text(key));
        }

        auto capture = std::make_unique<detail::FrameCapture<F>>(api_, std::move(function));
        detail::ErrorCopy error;
        if (api_.bind(keys.data(), fc::detail::count(keys.size()), static_cast<std::uint32_t>(mode), capture->callback(owner_, service_),
                      error.sink()) != 1) {
            return std::unexpected(error.take("Could not register Lua binding"));
        }

        capture.release();
        error.check();
        return {};
    }

    // Registers F(StateId, bool alive) during Prepare; the control thread reports live VMs after activation, then lifetime changes.
    // Synchronize data shared with callbacks on the Lua thread.
    template <class F> Result<> on_state(F function) {
        if (!api_.on_state) {
            return std::unexpected(Error{"Lua bindings are unavailable"});
        }

        auto capture = std::make_unique<detail::StateCapture<F>>(std::move(function));
        detail::ErrorCopy error;
        if (api_.on_state(capture->callback(owner_, service_), error.sink()) != 1) {
            return std::unexpected(error.take("Could not register Lua state listener"));
        }

        capture.release();
        error.check();
        return {};
    }

    // Runs F(Frame&) synchronously on this live VM's Lua thread; Lua errors return through Result.
    // Call at a native point where Lua execution is appropriate.
    template <class F> Result<> run(StateId state, F function) {
        if (!api_.run) {
            return std::unexpected(Error{"Lua bindings are unavailable"});
        }

        detail::FrameCapture<F> capture{api_, std::move(function)};
        detail::ErrorCopy error;
        if (api_.run(state.value, capture.callback(owner_, service_).invoke, error.sink()) != 1) {
            return std::unexpected(error.take("Could not run Lua callback"));
        }

        error.check();
        return {};
    }

    // Queues F(Frame&) to run on the VM's Lua thread during game updates; only Accepted transfers ownership.
    // Accepted work runs once or is canceled when its VM or consumer ends; the control thread destroys its capture.
    template <class F> DispatchResult defer(StateId state, F function) {
        if (!api_.defer) {
            return DispatchResult::Unavailable;
        }

        auto capture = std::make_unique<detail::FrameCapture<F>>(api_, std::move(function));
        const auto result = static_cast<DispatchResult>(api_.defer(state.value, capture->callback(owner_, service_)));
        if (result == DispatchResult::Accepted) {
            capture.release();
        }

        return result;
    }

  private:
    friend Result<Bindings> get(PrepareContext &);

    Bindings(detail::Api api, void *owner, const FC_PatchApi *service) noexcept : api_(api), owner_(owner), service_(service) {}

    detail::Api api_{};
    void *owner_{};
    const FC_PatchApi *service_{};
};

// Retrieves the core.lua_bindings API during Prepare and retains the consumer's callback identity.
inline Result<Bindings> get(PrepareContext &context) {
    auto api = context.get("core.lua_bindings", detail::contract());
    if (!api) {
        return std::unexpected(std::move(api.error()));
    }

    const auto consumer = context.callback(fc::Function<void() noexcept>{});
    return Bindings{*api, consumer.owner, consumer.api};
}

} // namespace fc::lua
