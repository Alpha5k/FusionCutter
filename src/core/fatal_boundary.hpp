#pragma once

#include <string_view>

namespace fc {

// The process-wide invariant boundary lets low-level installed callbacks reach the one
// framework-owned fatal path without depending on CoreRuntime or introducing a second termination policy.
using FatalHandler = void (*)(void* context, std::string_view reason) noexcept;

// CoreRuntime publishes its process-lifetime reporting callback before installed code can run.
void install_fatal_handler(void* context, FatalHandler handler) noexcept;

// Terminates with the framework fatal exit code after giving the installed handler one chance
// to publish already-owned diagnostics; this function never unwinds through plugin code.
[[noreturn]] void fatal_invariant(std::string_view reason) noexcept;

} // namespace fc
