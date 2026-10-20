#include "../startup.hpp"

#include <Windows.h>

#include <string>

namespace {

HMODULE loader_module{};

// Starts this DLL's embedded runtime outside loader lock; returns 1 on success or 0 with a synchronous error.
// The sink must copy any error message it retains after the callback.
extern "C" std::uint32_t FC_CDECL FC_Start(std::uint32_t, const FC_StartOptions *, FC_ErrorSink) noexcept;

// Starts the embedded dedicated runtime outside loader lock; startup failure terminates the process.
DWORD WINAPI bootstrap(void *) noexcept {
    // Classic injection may precede either game's DLL; wait on this worker outside loader lock.
#if defined(_WIN64)
    while (GetModuleHandleW(L"Battlefront1.dll") == nullptr && GetModuleHandleW(L"Battlefront2.dll") == nullptr) {
        Sleep(10);
    }
#endif

    const FC_StartOptions options{sizeof(FC_StartOptions), 1};
    fc::loaders::StartError error;
    // Enter this combined DLL's own FC_Start export with the dedicated launch mode.
    // Copy its diagnostic during the callback so failure reporting can use it after FC_Start returns.
    if (FC_Start(FC_ABI_VERSION, &options, {&error, &fc::loaders::StartError::copy}) != 1) {
        fc::loaders::fail_startup(loader_module, true, "RconServer", "Start Fusion Cutter",
                                  error.message.empty() ? "Runtime initialization failed" : error.message, error.windows_error);
    }
    return ERROR_SUCCESS;
}

} // namespace

// Save the loader's module handle and start a worker to initialize the dedicated runtime.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH) {
        loader_module = instance;
        DisableThreadLibraryCalls(instance);

        // Windows delays this worker until DLL notifications release loader lock; DllMain does not wait for it.
        // The injector must retain its LoadLibrary reference throughout the worker's lifetime.
        const auto worker = CreateThread(nullptr, 0, &bootstrap, nullptr, 0, nullptr);
        if (worker == nullptr) {
            return FALSE;
        }
        CloseHandle(worker);
    }
    return TRUE;
}
