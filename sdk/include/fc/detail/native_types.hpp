#pragma once

#include <fc/base.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <memory>
#include <functional>
#include <tuple>
#include <type_traits>
#include <vector>

namespace fc::detail {

// Lets the handler own heterogeneous callback objects and destroy them inside the plugin DLL.
struct CallbackCapture {
    virtual ~CallbackCapture() = default;
};

using CallbackCaptures = std::vector<std::unique_ptr<CallbackCapture>>;

// Copies synchronous ABI diagnostics into an owned SDK error, with a fallback when no message was supplied.
struct NativeError {
    Error value;

    // Return the captured diagnostic or a fallback when no message was supplied.
    Error failure() const {
        return value.message.empty() ? Error{"Native service is unavailable or returned no error message"} : value;
    }

    // The returned sink borrows this collector and must be used before it is destroyed.
    FC_ErrorSink sink() {
        return {this, [](void *context, FC_Text message, std::uint32_t code) {
                    static_cast<NativeError *>(context)->value = {std::string{view(message)}, code};
                }};
    }
};

// Describe the logical byte size and alignment of a trivially copyable argument or result.
template <class T> constexpr FC_NativeValue native_value() {
    if constexpr (std::is_void_v<T>) {
        return {FC_NATIVE_VOID, 0, 0};
    } else {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_reference_v<T>);
        constexpr auto kind = std::is_pointer_v<T>                           ? FC_NATIVE_POINTER
                              : std::is_same_v<T, float>                     ? FC_NATIVE_FLOAT_32
                              : std::is_same_v<T, double>                    ? FC_NATIVE_FLOAT_64
                              : (std::is_integral_v<T> || std::is_enum_v<T>) ? FC_NATIVE_INTEGER
                                                                             : FC_NATIVE_RECORD;
        return {kind, sizeof(T), alignof(T)};
    }
}

// Shares logical argument traits and noexcept frame invocation across compiler and explicit call descriptions.
template <class R, class... A> struct FunctionShape {
    using Result = R;
    using Arguments = std::tuple<A...>;
    static constexpr bool variadic = false;
    static constexpr std::uint32_t convention = 0;

    // Invoke a noexcept replacement from a matching logical frame using the indexed arguments.
    template <class F, std::size_t... I>
    static decltype(auto) apply(F &&callback, const FC_NativeFrame &frame, std::index_sequence<I...>) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<R, F, const A &...>,
                      "Native modifiers must accept the function arguments and return a compatible result without throwing (noexcept)");
        // Frame entries point to logical values, even when the native ABI originally passed them in registers.
        return std::invoke(std::forward<F>(callback), *static_cast<const A *>(frame.arguments[I])...);
    }

    template <class F> static decltype(auto) apply(F &&callback, const FC_NativeFrame &frame) noexcept {
        return apply(std::forward<F>(callback), frame, std::index_sequence_for<A...>{});
    }

    static constexpr std::array<FC_NativeValue, sizeof...(A)> values{native_value<A>()...};
};
// Extract the signature, variadic status and x86 calling convention from a function pointer.
template <class T> struct FunctionTraits;

template <class R, class... A> struct FunctionTraits<R (*)(A...)> : FunctionShape<R, A...> {};

template <class R, class... A> struct FunctionTraits<R (*)(A...) noexcept> : FunctionShape<R, A...> {};

template <class R, class... A> struct FunctionTraits<R (*)(A..., ...)> : FunctionShape<R, A...> {
    static constexpr bool variadic = true;
};

template <class R, class... A> struct FunctionTraits<R (*)(A..., ...) noexcept> : FunctionShape<R, A...> {
    static constexpr bool variadic = true;
};
#if defined(_M_IX86)
template <class R, class... A> struct FunctionTraits<R(__stdcall *)(A...)> : FunctionShape<R, A...> {
    static constexpr std::uint32_t convention = 1;
};

template <class R, class... A> struct FunctionTraits<R(__stdcall *)(A...) noexcept> : FunctionTraits<R(__stdcall *)(A...)> {};

template <class R, class... A> struct FunctionTraits<R(__fastcall *)(A...)> : FunctionShape<R, A...> {
    static constexpr std::uint32_t convention = 2;
};

template <class R, class... A> struct FunctionTraits<R(__fastcall *)(A...) noexcept> : FunctionTraits<R(__fastcall *)(A...)> {};

template <class R, class... A> struct FunctionTraits<R(__thiscall *)(A...)> : FunctionShape<R, A...> {
    static constexpr std::uint32_t convention = 3;
};

template <class R, class... A> struct FunctionTraits<R(__thiscall *)(A...) noexcept> : FunctionTraits<R(__thiscall *)(A...)> {};
#endif
// Owns argument descriptions while a temporary C view is submitted or copied by the runtime.
struct CallMetadata {
    FC_NativeCall call{};
    std::vector<FC_NativeArgument> arguments;

    // The argument pointer remains valid only while this metadata and its vector remain unchanged.
    FC_NativeCall view() const {
        auto value = call;
        value.arguments = arguments.data();
        value.argument_count = count(arguments.size());
        return value;
    }
};

// Derive owned compiler ABI metadata; bridged rejects variadic signatures and record arguments or results.
template <class Call> CallMetadata compiler_call(bool bridged) {
    using Traits = FunctionTraits<Call>;
    using R = typename Traits::Result;
    CallMetadata result;
#if defined(_WIN64)
    result.call.architecture = FC_ARCH_X64;
#else
    result.call.architecture = FC_ARCH_X86;
#endif
    result.call.flags = FC_CALL_COMPILER | (Traits::variadic ? FC_CALL_VARIADIC : 0);
    result.call.result = native_value<R>();
    // Direct calls leave record passing to the compiler; generated bridges need explicit physical storage.
    if (bridged) {
        for (const auto &argument : Traits::values) {
            if (argument.kind == FC_NATIVE_RECORD) {
                throw std::invalid_argument("Bridged record arguments require an explicit NativeCall description");
            }
        }
    }

    // A bridge needs a fixed argument list to place values in registers and stack slots.
    if (bridged && Traits::variadic) {
        throw std::invalid_argument("Variadic native calls require a direct call through a compiler function pointer");
    }

    // User records need an explicit physical result description for a bridge; direct calls use the compiler.
    if (bridged && result.call.result.kind == FC_NATIVE_RECORD) {
        throw std::invalid_argument("Bridged record results require an explicit NativeCall description");
    }

    std::uint32_t stack{};
#if defined(_WIN64)
    result.call.cleanup = FC_STACK_CLEANUP_NONE;
    // The first four logical positions use integer or XMM registers; later positions occupy eight-byte stack slots.
    constexpr std::array<std::uint32_t, 4> registers{FC_REGISTER_RCX, FC_REGISTER_RDX, FC_REGISTER_R8, FC_REGISTER_R9};
    std::size_t index{};
    for (auto value : Traits::values) {
        FC_NativeStorage home{};
        if (index < 4) {
            home = {FC_NATIVE_STORAGE_REGISTER,
                    (value.kind == FC_NATIVE_FLOAT_32 || value.kind == FC_NATIVE_FLOAT_64)
                        ? FC_REGISTER_XMM0 + static_cast<std::uint32_t>(index)
                        : registers[index],
                    0};
        } else {
            home = {FC_NATIVE_STORAGE_STACK, 0, stack};
            stack += 8;
        }
        result.arguments.push_back({value, home});
        ++index;
    }

    result.call.stack_size = stack;
    if constexpr (!std::is_void_v<R>) {
        result.call.return_storage = {FC_NATIVE_STORAGE_REGISTER, std::is_floating_point_v<R> ? FC_REGISTER_XMM0 : FC_REGISTER_RAX, 0};
    }
#else
    // stdcall, fastcall and thiscall use callee cleanup; cdecl keeps cleanup with the caller.
    result.call.cleanup = Traits::convention ? FC_STACK_CLEANUP_CALLEE : FC_STACK_CLEANUP_CALLER;
    std::uint32_t register_index{};
    std::size_t argument_index{};
    for (auto value : Traits::values) {
        FC_NativeStorage home{};
        // thiscall places its first argument in ECX; fastcall uses ECX/EDX for eligible four-byte values.
        if constexpr (Traits::convention == 3) {
            if (argument_index == 0) {
                home = {FC_NATIVE_STORAGE_REGISTER, FC_REGISTER_ECX, 0};
            }
        } else if constexpr (Traits::convention == 2) {
            if (register_index < 2 && value.kind != FC_NATIVE_RECORD && value.kind != FC_NATIVE_FLOAT_32 &&
                value.kind != FC_NATIVE_FLOAT_64 && value.size <= 4) {
                home = {FC_NATIVE_STORAGE_REGISTER, static_cast<std::uint32_t>(register_index++ ? FC_REGISTER_EDX : FC_REGISTER_ECX), 0};
            }
        }

        // Remaining x86 arguments occupy stack slots rounded up to four bytes.
        if (home.kind == FC_NATIVE_STORAGE_NONE) {
            home = {FC_NATIVE_STORAGE_STACK, 0, stack};
            stack += (value.size + 3) & ~3u;
        }
        result.arguments.push_back({value, home});
        ++argument_index;
    }

    result.call.stack_size = stack;
    if constexpr (!std::is_void_v<R>) {
        result.call.return_storage = {FC_NATIVE_STORAGE_REGISTER, std::is_floating_point_v<R> ? FC_REGISTER_ST0 : FC_REGISTER_EAX, 0};
    }
#endif

    // An x86 call with no stack arguments has no argument bytes for either side to clean up.
    if (result.call.architecture == FC_ARCH_X86 && !result.call.stack_size) {
        result.call.cleanup = FC_STACK_CLEANUP_NONE;
    }
    return result;
}
} // namespace fc::detail
