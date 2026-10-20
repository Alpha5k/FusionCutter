#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace {

std::atomic_uint32_t calls{};
std::atomic_uint32_t game_ext_calls{};
bool proxy_marker{};
} // namespace

// Records the marker while LoadLibrary runs, before the explicit GameExt initializer.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH) {
        proxy_marker = GetEnvironmentVariableA("BF2GAMEEXT_PROXY", nullptr, 0) != 0;
    }
    return TRUE;
}

// Accepts the same ABI as GameExt and verifies its INI and ordering before reporting initialization.
extern "C" BOOL WINAPI BF2GameExt_Init(std::uintptr_t exe_base, const char *ini_path) {
    char directory[32768]{};
    if (!GetEnvironmentVariableA("FC_TEST_LOADER_DIRECTORY", directory, sizeof(directory)) || !ini_path || !proxy_marker ||
        exe_base != reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) || GetModuleHandleW(L"FusionCutter.dll") ||
        std::filesystem::path{ini_path} != std::filesystem::path{directory} / "BF2GameExt.ini" ||
        GetPrivateProfileIntA("Fixture", "Value", 0, ini_path) != 42) {
        return FALSE;
    }
    // Keep concurrent first calls overlapped so the count detects repeated initialization.
    Sleep(25);
    if (GetModuleHandleW(L"FusionCutter.dll")) {
        return FALSE;
    }
    game_ext_calls.fetch_add(1, std::memory_order_relaxed);
    char mode[32]{};
    GetEnvironmentVariableA("FC_TEST_CHAIN_MODE", mode, sizeof(mode));
    return std::strcmp(mode, "gameext-failed-init") != 0;
}

// Returns the number of valid GameExt initialization requests.
extern "C" std::uint32_t FC_Test_GameExtCount() noexcept {
    return game_ext_calls.load(std::memory_order_relaxed);
}

// Checks forwarded DirectInput arguments or calls the loader recursively to exercise its system fallback.
extern "C" HRESULT WINAPI FC_Test_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID id, LPVOID *output,
                                                     IUnknown *outer) noexcept {
    calls.fetch_add(1, std::memory_order_relaxed);

    char mode[32]{};
    GetEnvironmentVariableA("FC_TEST_CHAIN_MODE", mode, sizeof(mode));
    if (std::strcmp(mode, "recursive") == 0) {
        // Reenter the proxy on the same thread so its recursion guard selects Windows DirectInput.
        using Entry = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, IUnknown *);
        const auto entry = reinterpret_cast<Entry>(GetProcAddress(GetModuleHandleW(L"dinput8.dll"), "DirectInput8Create"));
        return entry(instance, version, id, output, outer);
    }

    if (output) {
        *output = reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x1234));
    }
    return instance == GetModuleHandleW(nullptr) && version == 0x0800 && id == GUID{} && outer == nullptr ? S_OK : E_INVALIDARG;
}

extern "C" std::uint32_t FC_Test_ChainCount() noexcept {
    return calls.load(std::memory_order_relaxed);
}

// Returns a distinct result so tests can identify an optional proxy's secondary export.
extern "C" HRESULT WINAPI FC_Test_CanUnload() noexcept {
    return static_cast<HRESULT>(0x12345);
}

// Returns a marked format owned by this DLL so tests can identify joystick forwarding.
extern "C" LPCDIDATAFORMAT WINAPI FC_Test_Joystick() noexcept {
    static const DIDATAFORMAT format{.dwSize = sizeof(DIDATAFORMAT), .dwObjSize = sizeof(DIOBJECTDATAFORMAT), .dwFlags = 0x4321};
    return &format;
}
