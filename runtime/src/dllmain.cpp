#include <Windows.h>

// Disable thread notifications when the DLL loads; FC_Start performs runtime initialization.
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
