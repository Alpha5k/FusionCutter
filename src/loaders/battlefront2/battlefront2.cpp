#include "../common/startup.hpp"

#include <Windows.h>

#include <cstdint>
#include <mutex>

namespace {

using GameWinMainFn = std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint8_t*,
                                        std::uint32_t, std::uint64_t);

inline constexpr std::wstring_view kOriginalFilename = L"Battlefront2.original.dll";

// The original game module and entry become process-lifetime forwarding state after the first exported call.
struct OriginalGame {
    HMODULE module{};
    GameWinMainFn entry{};
    const char* failure_reason{"Battlefront2.original.dll could not be initialized"};
    DWORD windows_error{ERROR_GEN_FAILURE};
};

HMODULE loader_module{};
std::once_flag original_once;
OriginalGame original;
fc::loaders::StartupController startup;

void initialize_original() noexcept {
    try {
        // Resolve the proxy's own directory so launcher working-directory state cannot choose the original game DLL.
        const auto directory = fc::loaders::loader_directory(loader_module);
        if (!directory) {
            original.failure_reason = "The Battlefront2 proxy directory could not be resolved";
            original.windows_error = directory.error().windows_error;
            return;
        }
        const auto path = *directory / kOriginalFilename;
        // The original may resolve adjacent dependencies while system components load only from System32.
        original.module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (original.module == nullptr) {
            original.failure_reason = "Battlefront2.original.dll could not be loaded";
            original.windows_error = GetLastError();
            return;
        }
        // Forwarding is possible only through the reviewed unmangled GameWinMain contract at ordinal 1.
        original.entry = reinterpret_cast<GameWinMainFn>(GetProcAddress(original.module, "GameWinMain"));
        if (original.entry == nullptr) {
            FreeLibrary(original.module);
            original.module = nullptr;
            original.failure_reason = "Battlefront2.original.dll does not export GameWinMain";
            original.windows_error = ERROR_PROC_NOT_FOUND;
        }
    } catch (...) {
        if (original.module != nullptr) {
            FreeLibrary(original.module);
            original.module = nullptr;
        }
        original.entry = nullptr;
        original.failure_reason = "Loading Battlefront2.original.dll ended unexpectedly";
        original.windows_error = ERROR_GEN_FAILURE;
    }
}

[[nodiscard]] std::uint32_t forwarding_failure() noexcept {
    const FC_LoaderStartupInfo startup_info{.loader_kind = FC_LOADER_KIND_BATTLEFRONT2,
                                            .direct_input_chain = FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE};
    fc::loaders::write_fallback_status(loader_module, FC_HOST_ROLE_CLIENT, startup_info, "Load original game DLL",
                                       original.failure_reason, original.windows_error);
    return original.windows_error;
}

} // namespace

// The Classic client proxy starts Fusion Cutter only after it can forward the launcher's exact seven-argument call.
extern "C" std::uint32_t GameWinMain(std::uint64_t argument1, std::uint64_t argument2, std::uint64_t argument3,
                                     std::uint64_t argument4, std::uint8_t* argument5, std::uint32_t argument6,
                                     std::uint64_t argument7) noexcept {
    try {
        std::call_once(original_once, &initialize_original);
    } catch (...) {
        original.failure_reason = "The one-time loader for the original game DLL failed";
        original.windows_error = ERROR_GEN_FAILURE;
    }
    if (original.entry == nullptr) {
        return forwarding_failure();
    }

    // Loading the original first gives framework target recognition the final Classic process image set.
    const FC_LoaderStartupInfo startup_info{.loader_kind = FC_LOADER_KIND_BATTLEFRONT2,
                                            .direct_input_chain = FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE};
    static_cast<void>(startup.start(loader_module, FC_HOST_ROLE_CLIENT, startup_info));
    return original.entry(argument1, argument2, argument3, argument4, argument5, argument6, argument7);
}

// Process attach records only proxy identity for the first exported call and starts no work under loader lock.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        loader_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
