#pragma once

#include <fc/detail/native_types.hpp>

#include <algorithm>

namespace fc::abi {

// Each register tag supplies a physical storage location to Args, Result or HiddenResult.
#define FC_NATIVE_REGISTER_TAG(Name, Id)                                                                                                   \
    struct Name {                                                                                                                          \
        static constexpr FC_NativeStorage storage{FC_NATIVE_STORAGE_REGISTER, Id, 0};                                                      \
    };

FC_NATIVE_REGISTER_TAG(Eax, FC_REGISTER_EAX)
FC_NATIVE_REGISTER_TAG(Ebx, FC_REGISTER_EBX)
FC_NATIVE_REGISTER_TAG(Ecx, FC_REGISTER_ECX)
FC_NATIVE_REGISTER_TAG(Edx, FC_REGISTER_EDX)
FC_NATIVE_REGISTER_TAG(Esi, FC_REGISTER_ESI)
FC_NATIVE_REGISTER_TAG(Edi, FC_REGISTER_EDI)
FC_NATIVE_REGISTER_TAG(Ebp, FC_REGISTER_EBP)
FC_NATIVE_REGISTER_TAG(St0, FC_REGISTER_ST0)
FC_NATIVE_REGISTER_TAG(Rax, FC_REGISTER_RAX)
FC_NATIVE_REGISTER_TAG(Rbx, FC_REGISTER_RBX)
FC_NATIVE_REGISTER_TAG(Rcx, FC_REGISTER_RCX)
FC_NATIVE_REGISTER_TAG(Rdx, FC_REGISTER_RDX)
FC_NATIVE_REGISTER_TAG(Rsi, FC_REGISTER_RSI)
FC_NATIVE_REGISTER_TAG(Rdi, FC_REGISTER_RDI)
FC_NATIVE_REGISTER_TAG(Rbp, FC_REGISTER_RBP)
FC_NATIVE_REGISTER_TAG(R8, FC_REGISTER_R8)
FC_NATIVE_REGISTER_TAG(R9, FC_REGISTER_R9)
FC_NATIVE_REGISTER_TAG(R10, FC_REGISTER_R10)
FC_NATIVE_REGISTER_TAG(R11, FC_REGISTER_R11)
FC_NATIVE_REGISTER_TAG(R12, FC_REGISTER_R12)
FC_NATIVE_REGISTER_TAG(R13, FC_REGISTER_R13)
FC_NATIVE_REGISTER_TAG(R14, FC_REGISTER_R14)
FC_NATIVE_REGISTER_TAG(R15, FC_REGISTER_R15)
FC_NATIVE_REGISTER_TAG(Xmm0, FC_REGISTER_XMM0)
FC_NATIVE_REGISTER_TAG(Xmm1, FC_REGISTER_XMM1)
FC_NATIVE_REGISTER_TAG(Xmm2, FC_REGISTER_XMM2)
FC_NATIVE_REGISTER_TAG(Xmm3, FC_REGISTER_XMM3)
FC_NATIVE_REGISTER_TAG(Xmm4, FC_REGISTER_XMM4)
FC_NATIVE_REGISTER_TAG(Xmm5, FC_REGISTER_XMM5)
FC_NATIVE_REGISTER_TAG(Xmm6, FC_REGISTER_XMM6)
FC_NATIVE_REGISTER_TAG(Xmm7, FC_REGISTER_XMM7)
FC_NATIVE_REGISTER_TAG(Xmm8, FC_REGISTER_XMM8)
FC_NATIVE_REGISTER_TAG(Xmm9, FC_REGISTER_XMM9)
FC_NATIVE_REGISTER_TAG(Xmm10, FC_REGISTER_XMM10)
FC_NATIVE_REGISTER_TAG(Xmm11, FC_REGISTER_XMM11)
FC_NATIVE_REGISTER_TAG(Xmm12, FC_REGISTER_XMM12)
FC_NATIVE_REGISTER_TAG(Xmm13, FC_REGISTER_XMM13)
FC_NATIVE_REGISTER_TAG(Xmm14, FC_REGISTER_XMM14)
FC_NATIVE_REGISTER_TAG(Xmm15, FC_REGISTER_XMM15)
#undef FC_NATIVE_REGISTER_TAG

// Offset zero begins after the return address and, on x64, the 32-byte parameter home area.
template <std::uint32_t Offset> struct Stack {
    static constexpr FC_NativeStorage storage{FC_NATIVE_STORAGE_STACK, 0, Offset};
};

// Storage tags occur in logical argument order, including arguments passed in registers.
template <class... StorageTags> struct Args {
    static constexpr std::array<FC_NativeStorage, sizeof...(StorageTags)> storage{StorageTags::storage...};
};

// The result bytes occupy the named register, including a directly returned small record.
template <class StorageTag> struct Result {
    static constexpr auto storage = StorageTag::storage;
    static constexpr bool hidden = false;
};

// The named argument storage carries a caller-supplied pointer to the logical record result.
template <class StorageTag> struct HiddenResult {
    static constexpr auto storage = StorageTag::storage;
    static constexpr bool hidden = true;
};

// Select x86 conventions where the caller removes stack argument bytes after returning.
struct CallerCleanup {
    static constexpr auto value = FC_STACK_CLEANUP_CALLER;
};

// Select x86 conventions where the called function removes stack argument bytes.
struct CalleeCleanup {
    static constexpr auto value = FC_STACK_CLEANUP_CALLEE;
};

// Describe a call with no x86 argument cleanup, including the fixed x64 stack convention.
struct NoCleanup {
    static constexpr auto value = FC_STACK_CLEANUP_NONE;
};

// Select the actual x86 stack cleanup convention together with each argument and result location.
template <class Arguments, class Return = Result<Eax>, class Cleanup = CallerCleanup> struct X86 {
    using ArgumentsType = Arguments;
    using ResultType = Return;
    using CleanupType = Cleanup;
    static constexpr auto architecture = FC_ARCH_X86;
};

// x64 reserves the parameter home area and keeps stack cleanup with the caller.
template <class Arguments, class Return = Result<Rax>> struct X64 {
    using ArgumentsType = Arguments;
    using ResultType = Return;
    using CleanupType = NoCleanup;
    static constexpr auto architecture = FC_ARCH_X64;
};
} // namespace fc::abi

namespace fc::detail {

// Extract logical value types from an explicit function signature without compiler storage deduction.
template <class Signature> struct SignatureTraits;

template <class R, class... A> struct SignatureTraits<R(A...)> : FunctionShape<R, A...> {};

template <class R, class... A> struct SignatureTraits<R(A...) noexcept> : FunctionShape<R, A...> {};
} // namespace fc::detail

namespace fc {

// Describe a logical signature with X86/X64<Args<...>, Result<...>> or leading storage tags and cleanup.
// Compact descriptions place remaining arguments on the stack; full conventions may select a HiddenResult pointer.
template <class Signature, class... Description> struct NativeCall : detail::SignatureTraits<Signature> {
    // Builds owned metadata for submission; invalid compact argument counts or stack extents throw invalid_argument.
    static detail::CallMetadata description() {
        using Traits = detail::SignatureTraits<Signature>;
        using R = typename Traits::Result;
        detail::CallMetadata result;
        result.call.result = detail::native_value<R>();
        using First = std::tuple_element_t<0, std::tuple<Description...>>;
        // The full calling convention specifies a register or stack location for each argument and the result.
        if constexpr (sizeof...(Description) == 1 && requires { typename First::ArgumentsType; }) {
            using Convention = First;
            result.call.architecture = Convention::architecture;
            result.call.return_storage = Convention::ResultType::storage;
            if constexpr (Convention::ResultType::hidden) {
                result.call.flags |= FC_CALL_HIDDEN_RESULT;
            }
            result.call.cleanup = Convention::CleanupType::value;
            constexpr auto homes = Convention::ArgumentsType::storage;
            static_assert(homes.size() == Traits::values.size());
            for (std::size_t i = 0; i < homes.size(); ++i) {
                result.arguments.push_back({Traits::values[i], homes[i]});
            }
        } else {
            // Compact descriptions use this build's architecture and conventional result register.
#if defined(_WIN64)
            result.call.architecture = FC_ARCH_X64;
            result.call.cleanup = FC_STACK_CLEANUP_NONE;
            result.call.return_storage = {FC_NATIVE_STORAGE_REGISTER, std::is_floating_point_v<R> ? FC_REGISTER_XMM0 : FC_REGISTER_RAX, 0};
#else
            result.call.architecture = FC_ARCH_X86;
            result.call.cleanup = FC_STACK_CLEANUP_CALLER;
            result.call.return_storage = {FC_NATIVE_STORAGE_REGISTER, std::is_floating_point_v<R> ? FC_REGISTER_ST0 : FC_REGISTER_EAX, 0};
#endif

            // Storage tags describe leading arguments in order; cleanup tags select who removes x86 stack bytes.
            const auto add_home = [&]<class Home>() {
                if constexpr (requires { Home::storage; }) {
                    const auto index = result.arguments.size();
                    if (index == Traits::values.size()) {
                        throw std::invalid_argument("More register or stack locations were specified than the function has arguments");
                    }
                    result.arguments.push_back({Traits::values[index], Home::storage});
                } else if constexpr (requires { Home::value; }) {
                    result.call.cleanup = Home::value;
                } else {
                    static_assert(sizeof(Home) == 0, "A compact NativeCall description accepts storage tags and stack cleanup tags");
                }
            };
            (add_home.template operator()<Description>(), ...);

            std::uint32_t stack{};
            // The remaining logical arguments occupy inferred pointer-sized stack slots, starting at offset zero.
            for (auto index = result.arguments.size(); index < Traits::values.size(); ++index) {
                const auto value = Traits::values[index];
                const FC_NativeStorage home{FC_NATIVE_STORAGE_STACK, 0, stack};
                const auto next = static_cast<std::uint64_t>(stack) + ((static_cast<std::uint64_t>(value.size) + sizeof(void *) - 1) &
                                                                       ~(static_cast<std::uint64_t>(sizeof(void *)) - 1));
                if (next > UINT32_MAX) {
                    throw std::invalid_argument("Native stack argument area exceeds the 32-bit size limit");
                }
                stack = static_cast<std::uint32_t>(next);
                result.arguments.push_back({value, home});
            }
        }

        if constexpr (std::is_void_v<R>) {
            result.call.return_storage = {};
        }

        // Explicit stack locations can be sparse; retain the largest rounded extent needed by any argument.
        for (const auto &argument : result.arguments) {
            if (argument.storage.kind == FC_NATIVE_STORAGE_STACK) {
                const auto end = static_cast<std::uint64_t>(argument.storage.stack_offset) +
                                 ((static_cast<std::uint64_t>(argument.value.size) + sizeof(void *) - 1) &
                                  ~(static_cast<std::uint64_t>(sizeof(void *)) - 1));
                if (end > UINT32_MAX) {
                    throw std::invalid_argument("Native stack argument area exceeds the 32-bit size limit");
                }
                result.call.stack_size = std::max(result.call.stack_size, static_cast<std::uint32_t>(end));
            }
        }

        // A hidden-result pointer in stack storage must also fit inside the bridge's stack argument area.
        if (result.call.return_storage.kind == FC_NATIVE_STORAGE_STACK) {
            const auto end = static_cast<std::uint64_t>(result.call.return_storage.stack_offset) + sizeof(void *);
            if (end > UINT32_MAX) {
                throw std::invalid_argument("Native result stack storage exceeds the 32-bit size limit");
            }
            result.call.stack_size = std::max(result.call.stack_size, static_cast<std::uint32_t>(end));
        }

        if (result.call.architecture == FC_ARCH_X86 && !result.call.stack_size) {
            // With no stack argument bytes, neither side has cleanup work even if the convention selected it.
            result.call.cleanup = FC_STACK_CLEANUP_NONE;
        }
        return result;
    }
};
} // namespace fc

namespace fc::detail {

template <class Signature, class... Description>
struct FunctionTraits<NativeCall<Signature, Description...>> : SignatureTraits<Signature> {};

// Return an explicit physical layout or deduce compiler metadata; bridged restricts compiler signatures for hooks.
template <class Call> CallMetadata call_metadata(bool bridged) {
    if constexpr (requires { Call::description(); }) {
        return Call::description();
    } else {
        return compiler_call<Call>(bridged);
    }
}
} // namespace fc::detail
