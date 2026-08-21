#include <Windows.h>

// FusionCutter.dll needs no per-thread loader notifications; all substantial startup remains behind the core API.
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
