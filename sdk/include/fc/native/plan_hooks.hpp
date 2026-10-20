#pragma once

#include <fc/native/plan.hpp>
#include <fc/detail/native_layout.hpp>
#include <fc/detail/native_callbacks.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace fc {

template <class Call> inline NativeFunction<Call> Plan::function(Location location, Evidence evidence) const {
    try {
        // Direct compiler calls retain their compiler signature; explicit layouts request a generated frame invoker.
        auto metadata = detail::call_metadata<Call>(false);
        FC_Operation operation{};
        operation.kind = FC_OPERATION_CALL;
        operation.value.call = {location.view(), evidence.view(), metadata.view()};
        return {submit(operation), api()};
    } catch (const std::exception &error) {
        fail(error.what());
        return {};
    }
}

template <class Call> inline NativeFunction<Call> Plan::function(std::uint32_t rva, Evidence evidence) const {
    return function<Call>(at(rva), std::move(evidence));
}

template <class Call> inline NativeFunction<Call> Plan::function(const FunctionLocation<Call> &site) const {
    return function<Call>(at(site.rva, site.name), site.evidence);
}

template <class Call, class F> inline NativeFunction<Call> Plan::hook(Location location, F callback, Evidence evidence) const {
    return modifier<Call>(std::move(location), std::move(callback), std::move(evidence), FC_HOOK_FUNCTION);
}

template <class Call, class F> inline NativeFunction<Call> Plan::hook_call(Location location, F callback, Evidence evidence) const {
    return modifier<Call>(std::move(location), std::move(callback), std::move(evidence), FC_HOOK_CALL);
}

template <class Call, class F> inline NativeFunction<Call> Plan::hook(const FunctionLocation<Call> &site, F callback) const {
    return hook<Call>(at(site.rva, site.name), std::move(callback), site.evidence);
}

template <class F> inline void Plan::hook_instruction(Location location, F callback, Evidence evidence) const {
    static_assert(std::is_nothrow_invocable_v<F &, CpuContext &>, "Instruction modifiers must accept CpuContext & and be noexcept");
    // Adapt the frame's CPU pointer to the mutable register reference expected by instruction modifiers.
    auto wrapped = [callback = std::move(callback)](FC_CpuContext *cpu) mutable noexcept {
        std::invoke(callback, *cpu);
    };

    // Store the callable at a stable address used as the runtime callback context.
    auto capture = std::make_unique<detail::Modifier<void (*)(FC_CpuContext *), decltype(wrapped)>>(std::move(wrapped));
    FC_Operation operation{};
    operation.kind = FC_OPERATION_HOOK;
    operation.value.hook.location = location.view();
    operation.value.hook.evidence = evidence.view();
    operation.value.hook.scope = FC_HOOK_INSTRUCTION;
    operation.value.hook.context = capture.get();
    operation.value.hook.modifier = &decltype(capture)::element_type::invoke;
    if (!captures_) {
        fail("Native callback storage is unavailable");
        return;
    }

    // Transfer ownership to the handler before submission can retain the callback pointer.
    captures_->push_back(std::move(capture));
    submit(operation);
}

template <class Call, class State, class F> inline void Plan::observe(Location location, Before<F> callback, Evidence evidence) const {
    observer<Call, State>(std::move(location), std::move(callback.callback), nullptr, std::move(evidence), FC_HOOK_FUNCTION);
}

template <class Call, class State, class F> inline void Plan::observe(Location location, After<F> callback, Evidence evidence) const {
    observer<Call, State>(std::move(location), nullptr, std::move(callback.callback), std::move(evidence), FC_HOOK_FUNCTION);
}

template <class Call, class State, class B, class A>
inline void Plan::observe(Location location, Before<B> first, After<A> last, Evidence evidence) const {
    observer<Call, State>(std::move(location), std::move(first.callback), std::move(last.callback), std::move(evidence), FC_HOOK_FUNCTION);
}

template <class State, class Call, class B, class A>
inline void Plan::observe(const FunctionLocation<Call> &site, Before<B> first, After<A> last) const {
    observer<Call, State>(at(site.rva, site.name), std::move(first.callback), std::move(last.callback), site.evidence, FC_HOOK_FUNCTION);
}

template <class Call, class State, class F> inline void Plan::observe_call(Location location, Before<F> callback, Evidence evidence) const {
    observer<Call, State>(std::move(location), std::move(callback.callback), nullptr, std::move(evidence), FC_HOOK_CALL);
}

template <class Call, class State, class F> inline void Plan::observe_call(Location location, After<F> callback, Evidence evidence) const {
    observer<Call, State>(std::move(location), nullptr, std::move(callback.callback), std::move(evidence), FC_HOOK_CALL);
}

template <class Call, class State, class B, class A>
inline void Plan::observe_call(Location location, Before<B> first, After<A> last, Evidence evidence) const {
    observer<Call, State>(std::move(location), std::move(first.callback), std::move(last.callback), std::move(evidence), FC_HOOK_CALL);
}

template <class State, class Call, class B, class A>
inline void Plan::observe_call(const CallLocation<Call> &site, Before<B> first, After<A> last) const {
    observer<Call, State>(at(site.rva, site.name), std::move(first.callback), std::move(last.callback), site.evidence, FC_HOOK_CALL);
}

template <class Call, class F> inline NativeFunction<Call> Plan::hook_call(const CallLocation<Call> &site, F callback) const {
    return hook_call<Call>(at(site.rva, site.name), std::move(callback), site.evidence);
}

template <class State, class Call, class F> inline void Plan::observe(const FunctionLocation<Call> &site, Before<F> callback) const {
    observer<Call, State>(at(site.rva, site.name), std::move(callback.callback), nullptr, site.evidence, FC_HOOK_FUNCTION);
}

template <class State, class Call, class F> inline void Plan::observe(const FunctionLocation<Call> &site, After<F> callback) const {
    observer<Call, State>(at(site.rva, site.name), nullptr, std::move(callback.callback), site.evidence, FC_HOOK_FUNCTION);
}

template <class State, class Call, class F> inline void Plan::observe_call(const CallLocation<Call> &site, Before<F> callback) const {
    observer<Call, State>(at(site.rva, site.name), std::move(callback.callback), nullptr, site.evidence, FC_HOOK_CALL);
}

template <class State, class Call, class F> inline void Plan::observe_call(const CallLocation<Call> &site, After<F> callback) const {
    observer<Call, State>(at(site.rva, site.name), nullptr, std::move(callback.callback), site.evidence, FC_HOOK_CALL);
}

template <class State, class B, class A>
inline void Plan::observe_instruction(const CodeLocation &site, Before<B> first, After<A> last) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    observe_instruction<State>(std::move(location), std::move(first), std::move(last), site.evidence);
}

template <class F> inline void Plan::observe_instruction(const CodeLocation &site, Before<F> callback) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    observe_instruction(std::move(location), std::move(callback), site.evidence);
}

template <class F> inline void Plan::observe_instruction(const CodeLocation &site, After<F> callback) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    observe_instruction(std::move(location), std::move(callback), site.evidence);
}

template <class F> inline void Plan::hook_instruction(const CodeLocation &site, F callback) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    hook_instruction(std::move(location), std::move(callback), site.evidence);
}

inline ExpectedFault Plan::expect_fault(Location location, NativeException kind, Evidence evidence) const {
    // Fault mode reserves the instruction and records the exact access kind that the handle may recover.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_READ;
    operation.value.read = {location.view(), evidence.view(), 1, 1, 0, FC_READ_FAULT, static_cast<std::uint32_t>(kind), nullptr, {}};
    return {submit(operation), api()};
}

inline ExpectedFault Plan::expect_fault(const CodeLocation &site, NativeException kind) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    return expect_fault(std::move(location), kind, site.evidence);
}

template <class F> inline void Plan::observe_instruction(Location location, Before<F> callback, Evidence evidence) const {
    static_assert(std::is_nothrow_invocable_v<F &, const CpuContext &>,
                  "Instruction observers must accept const CpuContext & and be noexcept");
    // Present the saved CPU context as const while adapting it to the logical-frame pointer argument.
    auto wrapped = [callback = std::move(callback.callback)](FC_CpuContext *cpu) mutable noexcept {
        std::invoke(callback, static_cast<const CpuContext &>(*cpu));
    };
    observer<void (*)(CpuContext *), void>(std::move(location), std::move(wrapped), nullptr, std::move(evidence), FC_HOOK_INSTRUCTION);
}

template <class F> inline void Plan::observe_instruction(Location location, After<F> callback, Evidence evidence) const {
    static_assert(std::is_nothrow_invocable_v<F &, const CpuContext &>,
                  "Instruction observers must accept const CpuContext & and be noexcept");
    // After observes the context after modifiers, before the runtime resumes native instructions.
    auto wrapped = [callback = std::move(callback.callback)](FC_CpuContext *cpu) mutable noexcept {
        std::invoke(callback, static_cast<const CpuContext &>(*cpu));
    };
    observer<void (*)(CpuContext *), void>(std::move(location), nullptr, std::move(wrapped), std::move(evidence), FC_HOOK_INSTRUCTION);
}

template <class State, class B, class A>
inline void Plan::observe_instruction(Location location, Before<B> first, After<A> last, Evidence evidence) const {
    // Paired observers either share only the const CPU context or also share this invocation's State object.
    if constexpr (std::is_void_v<State>) {
        static_assert(std::is_nothrow_invocable_v<B &, const CpuContext &> && std::is_nothrow_invocable_v<A &, const CpuContext &>);
        auto first_callback = [callback = std::move(first.callback)](FC_CpuContext *cpu) mutable noexcept {
            std::invoke(callback, static_cast<const CpuContext &>(*cpu));
        };
        auto last_callback = [callback = std::move(last.callback)](FC_CpuContext *cpu) mutable noexcept {
            std::invoke(callback, static_cast<const CpuContext &>(*cpu));
        };
        observer<void (*)(CpuContext *), void>(std::move(location), std::move(first_callback), std::move(last_callback),
                                               std::move(evidence), FC_HOOK_INSTRUCTION);
    } else {
        // Before may fill State; after receives a const view of the same invocation's initialized object.
        static_assert(std::is_nothrow_invocable_v<B &, const CpuContext &, State &> &&
                      std::is_nothrow_invocable_v<A &, const CpuContext &, const State &>);
        auto before_callback = [callback = std::move(first.callback)](FC_CpuContext *cpu, State &state) mutable noexcept {
            std::invoke(callback, static_cast<const CpuContext &>(*cpu), state);
        };
        auto after_callback = [callback = std::move(last.callback)](FC_CpuContext *cpu, const State &state) mutable noexcept {
            std::invoke(callback, static_cast<const CpuContext &>(*cpu), state);
        };
        observer<void (*)(CpuContext *), State>(std::move(location), std::move(before_callback), std::move(after_callback),
                                                std::move(evidence), FC_HOOK_INSTRUCTION);
    }
}

template <class Call, class State, class F> inline void Plan::observe(std::uint32_t rva, Before<F> callback, Evidence evidence) const {
    observe<Call, State>(at(rva), std::move(callback), std::move(evidence));
}

template <class Call, class State, class F> inline void Plan::observe(std::uint32_t rva, After<F> callback, Evidence evidence) const {
    observe<Call, State>(at(rva), std::move(callback), std::move(evidence));
}

template <class Call, class State, class B, class A>
inline void Plan::observe(std::uint32_t rva, Before<B> first, After<A> last, Evidence evidence) const {
    observe<Call, State>(at(rva), std::move(first), std::move(last), std::move(evidence));
}

template <class Call, class State, class F> inline void Plan::observe_call(std::uint32_t rva, Before<F> callback, Evidence evidence) const {
    observe_call<Call, State>(at(rva), std::move(callback), std::move(evidence));
}

template <class Call, class State, class F> inline void Plan::observe_call(std::uint32_t rva, After<F> callback, Evidence evidence) const {
    observe_call<Call, State>(at(rva), std::move(callback), std::move(evidence));
}

template <class Call, class State, class B, class A>
inline void Plan::observe_call(std::uint32_t rva, Before<B> first, After<A> last, Evidence evidence) const {
    observe_call<Call, State>(at(rva), std::move(first), std::move(last), std::move(evidence));
}

template <class F> inline void Plan::observe_instruction(std::uint32_t rva, Before<F> callback, Evidence evidence) const {
    observe_instruction(at(rva), std::move(callback), std::move(evidence));
}

template <class F> inline void Plan::observe_instruction(std::uint32_t rva, After<F> callback, Evidence evidence) const {
    observe_instruction(at(rva), std::move(callback), std::move(evidence));
}

template <class State, class B, class A>
inline void Plan::observe_instruction(std::uint32_t rva, Before<B> first, After<A> last, Evidence evidence) const {
    observe_instruction<State>(at(rva), std::move(first), std::move(last), std::move(evidence));
}

template <class Call, class F> inline NativeFunction<Call> Plan::hook(std::uint32_t rva, F callback, Evidence evidence) const {
    return hook<Call>(at(rva), std::move(callback), std::move(evidence));
}

template <class Call, class F> inline NativeFunction<Call> Plan::hook_call(std::uint32_t rva, F callback, Evidence evidence) const {
    return hook_call<Call>(at(rva), std::move(callback), std::move(evidence));
}

template <class F> inline void Plan::hook_instruction(std::uint32_t rva, F callback, Evidence evidence) const {
    hook_instruction(at(rva), std::move(callback), std::move(evidence));
}

inline ExpectedFault Plan::expect_fault(std::uint32_t rva, NativeException kind, Evidence evidence) const {
    return expect_fault(at(rva), kind, std::move(evidence));
}

template <class Call, class F>
inline NativeFunction<Call> Plan::modifier(Location location, F callback, Evidence evidence, std::uint32_t scope) const {
    try {
        // Hook bridges convert native registers and stack slots into typed callback arguments.
        auto metadata = detail::call_metadata<Call>(true);
        auto call = metadata.view();
        auto capture = std::make_unique<detail::Modifier<Call, F>>(std::move(callback), api());

        FC_Operation operation{};
        operation.kind = FC_OPERATION_HOOK;
        operation.value.hook.location = location.view();
        operation.value.hook.evidence = evidence.view();
        operation.value.hook.scope = scope;
        operation.value.hook.call = &call;
        operation.value.hook.context = capture.get();
        operation.value.hook.modifier = &detail::Modifier<Call, F>::invoke;
        if (!captures_) {
            fail("Native callback storage is unavailable");
            return {};
        }

        // Keep the replacement callable alive in the handler and return the runtime's original-call resource.
        captures_->push_back(std::move(capture));
        return {submit(operation), api()};
    } catch (const std::exception &error) {
        fail(error.what());
        return {};
    }
}

template <class Call, class State, class B, class A>
inline void Plan::observer(Location location, B first, A last, Evidence evidence, std::uint32_t scope) const {
    try {
        // The runtime initializes State per invocation and releases its storage without calling a destructor.
        if constexpr (!std::is_void_v<State>) {
            static_assert(std::is_trivially_copyable_v<State> && std::is_nothrow_default_constructible_v<State>);
        }

        // Retain both observer callables together so their context addresses remain stable after planning returns.
        auto metadata = detail::call_metadata<Call>(true);
        auto call = metadata.view();
        using Capture = detail::Observer<Call, State, B, A>;
        auto capture = std::make_unique<Capture>(std::move(first), std::move(last), api());

        FC_Operation operation{};
        operation.kind = FC_OPERATION_HOOK;
        auto &hook = operation.value.hook;
        hook.location = location.view();
        hook.evidence = evidence.view();
        hook.scope = scope;
        hook.call = scope == FC_HOOK_INSTRUCTION ? nullptr : &call;
        hook.context = capture.get();
        if constexpr (!std::is_same_v<B, std::nullptr_t>) {
            hook.before = &Capture::first;
        }
        if constexpr (!std::is_same_v<A, std::nullptr_t>) {
            hook.after = &Capture::last;
        }
        if constexpr (!std::is_void_v<State>) {
            hook.state_size = sizeof(State);
            hook.state_alignment = alignof(State);
            hook.initialize = &Capture::initialize;
        }

        if (!captures_) {
            fail("Native callback storage is unavailable");
            return;
        }

        // Transfer ownership before the runtime copies and retains the hook's callback/context pointers.
        captures_->push_back(std::move(capture));
        submit(operation);
    } catch (const std::exception &error) {
        fail(error.what());
    }
}

} // namespace fc
