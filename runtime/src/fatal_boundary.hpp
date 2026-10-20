#pragma once

#include <fc/abi.h>

#include <string_view>
#include <memory>
#include <type_traits>

namespace fc {

// Reports a fatal error synchronously on the failing thread before process termination.
using FatalHandler = void (*)(void *context, std::string_view reason, std::uint32_t windows_error) noexcept;

// Register before callbacks can fail; registration must not race with other registration or fatal handling.
// The callback and its context remain valid for the process lifetime.
void install_fatal_handler(void *context, FatalHandler handler) noexcept;

// Reports the first fatal attempt, then terminates; recursive and concurrent attempts terminate without reporting.
[[noreturn]] void fatal_invariant(std::string_view reason, std::uint32_t windows_error = 0) noexcept;

// Runs the callback synchronously on this thread; callback and context must remain valid through the call.
// Escaping native faults terminate even if surrounding game code could catch them.
void invoke_patch_callback(FC_ConsumerInvoke callback, void *context) noexcept;

// Adapts a callable to the synchronous callback function above; the callable lives through this call.
template <class F> void invoke_patch_callback(F &&callback) noexcept {
    invoke_patch_callback(
        [](void *context) {
            (*static_cast<std::remove_reference_t<F> *>(context))();
        },
        std::addressof(callback));
}

} // namespace fc
