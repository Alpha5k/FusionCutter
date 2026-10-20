#pragma once

#include <fc/detail/native_types.hpp>
#include <fc/native/context.hpp>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace fc::detail {

// Supplies call context only when the callback requests it; older runtimes leave its return address unavailable.
template <class R, class F, class... A> R invoke_native_callback(const FC_NativeApi *api, F &callback, A &&...arguments) noexcept {
    if constexpr (std::is_nothrow_invocable_r_v<R, F &, A...>) {
        return std::invoke_r<R>(callback, std::forward<A>(arguments)...);
    } else {
        static_assert(std::is_nothrow_invocable_r_v<R, F &, const NativeContext &, A...>,
                      "Native callbacks must match the declared arguments and be noexcept; NativeContext is optional");
        const auto address =
            table_available(api, offsetof(FC_NativeApi, return_address), sizeof(api->return_address)) && api->return_address
                ? api->return_address()
                : 0;
        const NativeContext context{address};
        return std::invoke_r<R>(callback, context, std::forward<A>(arguments)...);
    }
}

// Retains a typed replacement callback and adapts the runtime's logical frame to its C++ signature.
template <class Call, class F> struct Modifier : CallbackCapture {
    F callback;
    const FC_NativeApi *api{};

    explicit Modifier(F value, const FC_NativeApi *native = nullptr) : callback(std::move(value)), api(native) {}

    // Invoke the retained replacement with a frame matching Call; context must refer to this live capture.
    static void FC_CDECL invoke(void *context, const FC_NativeFrame *frame, void *) noexcept {
        auto &self = *static_cast<Modifier *>(context);
        using Traits = FunctionTraits<Call>;
        using R = typename Traits::Result;
        auto invoke = [&](const auto &...arguments) noexcept -> R {
            return invoke_native_callback<R>(self.api, self.callback, arguments...);
        };
        // Non-void replacements write the logical result that the bridge returns to native code.
        if constexpr (std::is_void_v<R>) {
            Traits::apply(invoke, *frame);
        } else {
            *static_cast<R *>(frame->result) = Traits::apply(invoke, *frame);
        }
    }
};

// Retains optional before/after callbacks; the runtime supplies separate State storage for each invocation.
template <class Call, class State, class B, class A> struct Observer : CallbackCapture {
    B before;
    A after;
    const FC_NativeApi *api{};

    Observer(B first, A last, const FC_NativeApi *native = nullptr) : before(std::move(first)), after(std::move(last)), api(native) {}

    using Traits = FunctionTraits<Call>;
    using R = typename Traits::Result;
    using Arguments = typename Traits::Arguments;

    // Invoke a present observer with const logical arguments, an optional completed result and invocation State.
    template <bool AfterCall, class F, std::size_t... I>
    static void apply(const FC_NativeApi *api, F &callback, const FC_NativeFrame &frame, void *state, std::index_sequence<I...>) noexcept {
        if constexpr (!std::is_same_v<F, std::nullptr_t>) {
            // After callbacks for non-void calls receive the result after the logical arguments.
            if constexpr (AfterCall && !std::is_void_v<R>) {
                if constexpr (std::is_void_v<State>) {
                    invoke_native_callback<void>(api, callback,
                                                 *static_cast<const std::tuple_element_t<I, Arguments> *>(frame.arguments[I])...,
                                                 *static_cast<const R *>(frame.result));
                } else {
                    invoke_native_callback<void>(api, callback,
                                                 *static_cast<const std::tuple_element_t<I, Arguments> *>(frame.arguments[I])...,
                                                 *static_cast<const R *>(frame.result), *static_cast<const State *>(state));
                }
            } else {
                // Before callbacks may fill State; after callbacks receive it as const alongside const arguments.
                if constexpr (std::is_void_v<State>) {
                    invoke_native_callback<void>(api, callback,
                                                 *static_cast<const std::tuple_element_t<I, Arguments> *>(frame.arguments[I])...);
                } else if constexpr (AfterCall) {
                    invoke_native_callback<void>(api, callback,
                                                 *static_cast<const std::tuple_element_t<I, Arguments> *>(frame.arguments[I])...,
                                                 *static_cast<const State *>(state));
                } else {
                    invoke_native_callback<void>(api, callback,
                                                 *static_cast<const std::tuple_element_t<I, Arguments> *>(frame.arguments[I])...,
                                                 *static_cast<State *>(state));
                }
            }
        }
    }

    // Adapt the runtime's before thunk; context refers to this live capture and state to initialized State when requested.
    static void FC_CDECL first(void *context, const FC_NativeFrame *frame, void *state) noexcept {
        auto &self = *static_cast<Observer *>(context);
        apply<false>(self.api, self.before, *frame, state, std::make_index_sequence<std::tuple_size_v<Arguments>>{});
    }

    // Adapt the after thunk with the completed result and the same invocation's State, when present.
    static void FC_CDECL last(void *context, const FC_NativeFrame *frame, void *state) noexcept {
        auto &self = *static_cast<Observer *>(context);
        apply<true>(self.api, self.after, *frame, state, std::make_index_sequence<std::tuple_size_v<Arguments>>{});
    }

    // Begin the C++ State lifetime in sufficient aligned runtime storage; the runtime releases it without destruction.
    static void FC_CDECL initialize(void *state) noexcept
        requires(!std::is_void_v<State>)
    {
        std::construct_at(static_cast<State *>(state));
    }
};

} // namespace fc::detail
