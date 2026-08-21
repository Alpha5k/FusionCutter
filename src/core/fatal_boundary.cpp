#include "fatal_boundary.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdlib>

namespace fc {
namespace {

inline constexpr UINT kFatalExitCode = 0xD1;

// Context is stored before the handler is published with release semantics and remains valid for process life.
std::atomic<void*> fatal_context{};
std::atomic<FatalHandler> fatal_handler{};
std::atomic_flag fatal_started = ATOMIC_FLAG_INIT;

[[noreturn]] void terminate_process() noexcept {
    TerminateProcess(GetCurrentProcess(), kFatalExitCode);
    // TerminateProcess can report failure, but returning into corrupted installed state is never legal.
    std::abort();
}

} // namespace

void install_fatal_handler(void* context, FatalHandler handler) noexcept {
    fatal_context.store(context, std::memory_order_relaxed);
    fatal_handler.store(handler, std::memory_order_release);
}

[[noreturn]] void fatal_invariant(std::string_view reason) noexcept {
    // Recursive fatal handling bypasses reporting because its state may be the source of the second failure.
    if (!fatal_started.test_and_set(std::memory_order_acq_rel)) {
        if (const auto handler = fatal_handler.load(std::memory_order_acquire); handler != nullptr) {
            handler(fatal_context.load(std::memory_order_relaxed), reason);
        }
    }
    terminate_process();
}

} // namespace fc
