#include "startup.hpp"
#include <array>
#include <exception>
#include <fstream>
#include <format>

namespace fc::loaders {
namespace {

// Reports a loader failure to the debugger, using a fixed message if formatting throws.
void debug_error(std::string_view reason, DWORD error) noexcept {
    try {
        const auto text = std::format("Fusion Cutter loader: {} (Windows error {})\r\n", reason, error);
        OutputDebugStringA(text.c_str());
    } catch (...) {
        OutputDebugStringA("Fusion Cutter loader reporting failed.\r\n");
    }
}

// Loads and starts the adjacent runtime outside loader lock; reported failures terminate the process.
// C++ exceptions propagate to StartupController's failure handler.
void load_runtime(HMODULE loader, bool dedicated, std::string_view name) {
    auto directory = loader_directory(loader);
    if (!directory) {
        fail_startup(loader, dedicated, name, "Resolve loader directory", directory.error().message, directory.error().windows_error);
    }

    // Retain the loaded DLL for the process lifetime so its runtime and callbacks remain available.
    const auto file = *directory / L"FusionCutter.dll";
    const auto runtime = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) {
        fail_startup(loader, dedicated, name, "Load FusionCutter.dll", "Could not load FusionCutter.dll from the loader's directory",
                     GetLastError());
    }

    const auto start = reinterpret_cast<FC_StartFn>(GetProcAddress(runtime, "FC_Start"));
    if (!start) {
        fail_startup(loader, dedicated, name, "Resolve FC_Start", "The runtime does not export FC_Start", ERROR_PROC_NOT_FOUND);
    }

    const FC_StartOptions options{sizeof(FC_StartOptions), dedicated ? 1u : 0u};
    StartError error;
    // Copy the diagnostic while FC_Start's error callback arguments are still valid.
    if (start(FC_ABI_VERSION, &options, {&error, &StartError::copy}) != 1) {
        fail_startup(loader, dedicated, name, "Start Fusion Cutter",
                     error.message.empty() ? "Runtime initialization failed" : error.message, error.windows_error);
    }
}
} // namespace

std::expected<std::filesystem::path, StartError> loader_directory(HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) {
        return std::unexpected(StartError{"The loader module path could not be read", size ? ERROR_INSUFFICIENT_BUFFER : GetLastError()});
    }
    return std::filesystem::path{std::wstring_view{path.data(), size}}.parent_path();
}

void write_fallback_status(HMODULE module, bool dedicated, std::string_view loader, std::string_view operation, std::string_view reason,
                           DWORD error) noexcept {
    debug_error(reason, error);
    try {
        const auto directory = loader_directory(module);
        if (!directory) {
            return;
        }

        std::ofstream output{*directory / L"FusionCutter.txt", std::ios::binary | std::ios::trunc};
        // Stream failures have no associated GetLastError; keep the originating startup error separate.
        if (!output) {
            debug_error("The loader status file could not be opened", ERROR_SUCCESS);
            return;
        }

        output << "Fusion Cutter loader\r\n  Loader: " << loader << "\r\n  Role: " << (dedicated ? "Server" : "Client")
               << "\r\n\r\nIssues\r\n  " << operation << ": Failed\r\n  Reason: " << reason << "\r\n";
        if (error) {
            output << "  Windows error: " << error << "\r\n";
        }

        output.flush();
        if (!output) {
            debug_error("The loader status file could not be written", ERROR_SUCCESS);
        }
    } catch (...) {
        debug_error("The loader status report could not be written", ERROR_SUCCESS);
    }
}

[[noreturn]] void fail_startup(HMODULE module, bool dedicated, std::string_view loader, std::string_view operation, std::string_view reason,
                               DWORD error) noexcept {
    write_fallback_status(module, dedicated, loader, operation, reason, error);
    TerminateProcess(GetCurrentProcess(), 0xd1);
    std::terminate();
}

void StartupController::start(HMODULE module, bool dedicated, std::string_view loader) noexcept {
    try {
        // Publish completed runtime startup before any waiting caller resumes its game or DirectInput entry.
        std::call_once(once_, [&] {
            load_runtime(module, dedicated, loader);
        });
    } catch (const std::exception &error) {
        fail_startup(module, dedicated, loader, "Start Fusion Cutter", error.what());
    } catch (...) {
        fail_startup(module, dedicated, loader, "Start Fusion Cutter", "Loader startup failed unexpectedly");
    }
}
} // namespace fc::loaders
