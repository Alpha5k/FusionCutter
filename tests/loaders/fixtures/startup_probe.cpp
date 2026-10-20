#include <fc/plugin.hpp>
#include <Windows.h>
#include <atomic>

namespace {

std::atomic_uint32_t callback_thread{};

// Builds a plugin with no patches so the child can test discovery independently of patch installation.
fc::Plugin build_plugin() {
    return fc::Plugin{.id = "startup_probe", .version = "1"};
}
} // namespace

// Records the discovery thread and returns 1 with a DLL-owned declaration through out on success.
// Delegates ABI validation and failure reporting to the SDK.
FC_PLUGIN_ENTRY
extern "C" FC_EXPORT std::uint32_t FC_CDECL FC_GetPlugin(std::uint32_t abi, const FC_Plugin **out, FC_ErrorSink error) {
    // Publish the callback thread before building the descriptor so the child can observe discovery.
    callback_thread.store(GetCurrentThreadId(), std::memory_order_release);
    return fc::detail::get_plugin<&build_plugin>(abi, out, error);
}

// Returns the last discovery callback's thread ID, or zero before the callback runs.
extern "C" FC_EXPORT std::uint32_t FC_Test_PluginThread() noexcept {
    return callback_thread.load(std::memory_order_acquire);
}
