#include <Windows.h>

extern "C" HRESULT WINAPI FC_Test_DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID*, void*) noexcept {
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
