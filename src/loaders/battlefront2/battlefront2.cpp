#include "core_loader.hpp"

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <utility>

namespace {

using GameWinMainFn = std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint8_t*,
                                        std::uint32_t, std::uint64_t);

constexpr auto kOriginalFilename = L"Battlefront2.original.dll";
constexpr auto kOriginalRecovery = "Restore a compatible Battlefront2.original.dll beside the Fusion Cutter proxy.";

struct OriginalGame {
    HMODULE module{};
    GameWinMainFn entry{};
    const char* failure_reason{"Battlefront2.original.dll could not be initialized"};
    DWORD windows_error{ERROR_GEN_FAILURE};
};

HMODULE g_loader_module{};
INIT_ONCE g_original_once = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_core_once = INIT_ONCE_STATIC_INIT;
OriginalGame g_original;
std::optional<fusioncutter::loaders::CoreConnection> g_core;

BOOL CALLBACK initialize_original_once(INIT_ONCE*, void*, void**) noexcept {
    try {
        const auto directory = fusioncutter::loaders::loader_directory(g_loader_module);
        if (!directory) {
            g_original.failure_reason = "The Battlefront2 proxy directory could not be resolved";
            g_original.windows_error = directory.error().windows_error;
            return TRUE;
        }

        const auto original_path = *directory / kOriginalFilename;
        g_original.module = LoadLibraryExW(original_path.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (g_original.module == nullptr) {
            g_original.failure_reason = "Battlefront2.original.dll could not be loaded";
            g_original.windows_error = GetLastError();
            return TRUE;
        }

        g_original.entry = reinterpret_cast<GameWinMainFn>(GetProcAddress(g_original.module, "GameWinMain"));
        if (g_original.entry == nullptr) {
            FreeLibrary(g_original.module);
            g_original.module = nullptr;
            g_original.failure_reason = "Battlefront2.original.dll does not export GameWinMain";
            g_original.windows_error = ERROR_PROC_NOT_FOUND;
        }
    } catch (...) {
        if (g_original.module != nullptr) {
            FreeLibrary(g_original.module);
            g_original.module = nullptr;
        }
        g_original.entry = nullptr;
        g_original.failure_reason = "Loading Battlefront2.original.dll ended unexpectedly";
        g_original.windows_error = ERROR_GEN_FAILURE;
    }
    return TRUE;
}

BOOL CALLBACK initialize_core_once(INIT_ONCE*, void*, void**) noexcept {
    FC_LoaderStartupInfo startup_info{
        .struct_size = sizeof(FC_LoaderStartupInfo),
        .loader_kind = FC_LOADER_KIND_BATTLEFRONT2,
        .direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE,
    };
    auto connection = fusioncutter::loaders::initialize_core(g_loader_module, FC_HOST_ROLE_CLIENT, startup_info);
    if (!connection) {
        fusioncutter::loaders::write_fallback_status(g_loader_module, FC_HOST_ROLE_CLIENT, connection.error().message,
                                                     connection.error().windows_error);
        return TRUE;
    }
    g_core.emplace(std::move(*connection));
    return TRUE;
}

[[nodiscard]] std::uint32_t forwarding_failure() noexcept {
    fusioncutter::loaders::write_fallback_status(g_loader_module, FC_HOST_ROLE_CLIENT, g_original.failure_reason,
                                                 g_original.windows_error, kOriginalRecovery);
    return g_original.windows_error;
}

} // namespace

extern "C" std::uint32_t GameWinMain(std::uint64_t argument1, std::uint64_t argument2, std::uint64_t argument3,
                                     std::uint64_t argument4, std::uint8_t* argument5, std::uint32_t argument6,
                                     std::uint64_t argument7) noexcept {
    InitOnceExecuteOnce(&g_original_once, initialize_original_once, nullptr, nullptr);
    if (g_original.entry == nullptr) {
        return forwarding_failure();
    }

    InitOnceExecuteOnce(&g_core_once, initialize_core_once, nullptr, nullptr);
    return g_original.entry(argument1, argument2, argument3, argument4, argument5, argument6, argument7);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_loader_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
