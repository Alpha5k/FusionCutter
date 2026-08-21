#include "../common/startup.hpp"

#include <Windows.h>

#include <atomic>

namespace {

HMODULE loader_module{};
std::atomic_bool running{};
fc::loaders::StartupController startup;

[[nodiscard]] bool game_image_ready() noexcept {
#if defined(_WIN64)
    // Classic server injection may precede its game DLL; framework target recognition waits for that image.
    return GetModuleHandleW(L"Battlefront2.dll") != nullptr;
#else
    return true;
#endif
}

// The bootstrap worker waits for final image state and then enters the common server initialization path once.
DWORD WINAPI bootstrap(void*) noexcept {
    // Classic injection may arrive before Battlefront2.dll, so its worker waits outside loader lock for final state.
    while (running.load(std::memory_order_acquire) && !game_image_ready()) {
        Sleep(10);
    }
    if (!running.load(std::memory_order_acquire)) {
        return ERROR_SUCCESS;
    }
    // The common helper owns framework API negotiation and the permanent pump; this shim carries loader identity.
    const FC_LoaderStartupInfo startup_info{.loader_kind = FC_LOADER_KIND_RCONSERVER,
                                            .direct_input_chain = FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE};
    static_cast<void>(startup.start(loader_module, FC_HOST_ROLE_SERVER, startup_info));
    return ERROR_SUCCESS;
}

} // namespace

// DllMain limits process attach to durable identity and worker creation so startup runs outside loader lock.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        loader_module = instance;
        DisableThreadLibraryCalls(instance);
        running.store(true, std::memory_order_release);

        // The injector's original LoadLibrary reference remains retained; this worker performs all substantial work
        // after loader lock without racing to pin its own module.
        const auto worker = CreateThread(nullptr, 0, &bootstrap, nullptr, 0, nullptr);
        if (worker == nullptr) {
            running.store(false, std::memory_order_release);
            return FALSE;
        }
        CloseHandle(worker);
    } else if (reason == DLL_PROCESS_DETACH) {
        running.store(false, std::memory_order_release);
    }
    return TRUE;
}
