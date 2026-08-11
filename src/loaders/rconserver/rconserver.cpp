#include "core_loader.hpp"

#include <Windows.h>

#include <atomic>

namespace {

constexpr DWORD kFatalExitCode = 0xD1;
constexpr DWORD kUpdateIntervalMilliseconds = 50;

HMODULE g_loader_module{};
std::atomic_bool g_running{};

[[nodiscard]] bool game_image_ready() noexcept {
#if defined(_WIN64)
    return GetModuleHandleW(L"Battlefront2.dll") != nullptr;
#else
    return true;
#endif
}

DWORD WINAPI run(void*) noexcept {
    while (g_running.load(std::memory_order_acquire) && !game_image_ready()) {
        Sleep(10);
    }
    if (!g_running.load(std::memory_order_acquire)) {
        return ERROR_SUCCESS;
    }

    FC_LoaderStartupInfo startup_info{
        .struct_size = sizeof(FC_LoaderStartupInfo),
        .loader_kind = FC_LOADER_KIND_RCONSERVER,
        .direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE,
    };
    auto connection = fusioncutter::loaders::initialize_core(g_loader_module, FC_HOST_ROLE_SERVER, startup_info);
    if (!connection) {
        fusioncutter::loaders::write_fallback_status(g_loader_module, FC_HOST_ROLE_SERVER, connection.error().message,
                                                     connection.error().windows_error);
        return connection.error().windows_error;
    }

    if (connection->initialization_result == FC_INIT_FATAL) {
        static_cast<void>(TerminateProcess(GetCurrentProcess(), kFatalExitCode));
        return kFatalExitCode;
    }
    if (connection->initialization_result != FC_INIT_COMPLETED) {
        return ERROR_SUCCESS;
    }

    while (g_running.load(std::memory_order_acquire)) {
        connection->api.update();
        Sleep(kUpdateIntervalMilliseconds);
    }
    return ERROR_SUCCESS;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_loader_module = instance;
        DisableThreadLibraryCalls(instance);
        g_running.store(true, std::memory_order_release);

        const auto worker = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
        if (worker == nullptr) {
            g_running.store(false, std::memory_order_release);
            return FALSE;
        }
        CloseHandle(worker);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running.store(false, std::memory_order_release);
    }
    return TRUE;
}
