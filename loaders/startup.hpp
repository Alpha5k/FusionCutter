#pragma once

#include <fc/abi.h>
#include <Windows.h>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace fc::loaders {

// Owns the startup diagnostic and Windows error code copied through FC_ErrorSink.
struct StartError {
    std::string message;
    std::uint32_t windows_error{};

    // context must point to a StartError; copies the borrowed callback text before returning.
    // Records ERROR_NOT_ENOUGH_MEMORY if copying throws.
    static void FC_CDECL copy(void *context, FC_Text text, std::uint32_t code) noexcept {
        auto &self = *static_cast<StartError *>(context);
        try {
            self.message.assign(text.data ? text.data : "", text.data ? text.size : 0);
            self.windows_error = code;
        } catch (...) {
            self.windows_error = ERROR_NOT_ENOUGH_MEMORY;
        }
    }
};

// Returns this loader's absolute directory or a path-read error, independently of the game's working directory.
std::expected<std::filesystem::path, StartError> loader_directory(HMODULE module);
// Reports startup failure to the debugger and replaces FusionCutter.txt beside the loader when writable.
// Contains reporting failures so fatal startup handling can continue.
void write_fallback_status(HMODULE module, bool dedicated, std::string_view loader, std::string_view operation, std::string_view reason,
                           DWORD windows_error = ERROR_SUCCESS) noexcept;

// Reports a fatal startup failure and terminates the process with exit code 0xD1.
[[noreturn]] void fail_startup(HMODULE module, bool dedicated, std::string_view loader, std::string_view operation, std::string_view reason,
                               DWORD windows_error = ERROR_SUCCESS) noexcept;

// Loads the adjacent runtime DLL and coordinates its first startup across calling threads.
class StartupController {
  public:
    // Call outside loader lock; the first call selects the runtime directory and role for this controller.
    // Concurrent calls wait for startup; failures report and terminate the process.
    void start(HMODULE module, bool dedicated, std::string_view loader) noexcept;

  private:
    std::once_flag once_;
};
} // namespace fc::loaders
