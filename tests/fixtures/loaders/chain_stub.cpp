#include <Windows.h>
#include <Unknwn.h>

// The chain accepts the real DirectInput signature so the proxy test proves forwarding without invoking DirectInput.
extern "C" HRESULT WINAPI FC_Test_DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID* output, IUnknown*) noexcept {
    if (output != nullptr) {
        *output = nullptr;
    }
    return S_OK;
}
