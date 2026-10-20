#include "fatal_boundary.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdlib>

namespace fc {

namespace {

inline constexpr UINT kFatalExitCode = 0xD1;

std::atomic<void *> fatal_context{};
std::atomic<FatalHandler> fatal_handler{};
std::atomic_flag fatal_started = ATOMIC_FLAG_INIT;

// Terminates the process with the runtime's fatal exit code.
[[noreturn]] void terminate_process() noexcept {
    TerminateProcess(GetCurrentProcess(), kFatalExitCode);

    // Fall back to abort if Windows cannot terminate the process.
    std::abort();
}

} // namespace

void install_fatal_handler(void *context, FatalHandler handler) noexcept {
    // Publish context before the handler so acquiring readers see the initialized callback context.
    fatal_context.store(context, std::memory_order_relaxed);
    fatal_handler.store(handler, std::memory_order_release);
}

[[noreturn]] void fatal_invariant(std::string_view reason, std::uint32_t windows_error) noexcept {
    // Recursive and concurrent fatal attempts bypass reporting and terminate immediately.
    if (!fatal_started.test_and_set(std::memory_order_acq_rel)) {
        if (const auto handler = fatal_handler.load(std::memory_order_acquire); handler != nullptr) {
            handler(fatal_context.load(std::memory_order_relaxed), reason, windows_error);
        }
    }

    terminate_process();
}

void invoke_patch_callback(FC_ConsumerInvoke callback, void *context) noexcept {
    // Catch escaping native faults here before any surrounding game handler can recover from the author failure.
    __try {
        callback(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fatal_invariant("Patch callback raised a native fault", GetExceptionCode());
    }
}

} // namespace fc
