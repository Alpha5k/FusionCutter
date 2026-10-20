#include "../startup.hpp"

#include <Windows.h>

#include <cstdint>
#include <mutex>
#include <string_view>

namespace {

using GameWinMainFn = std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint8_t *, std::uint32_t,
                                        std::uint64_t);

inline constexpr std::string_view kLoaderName = FC_CLASSIC_GAME_NAME;
inline constexpr std::wstring_view kOriginalFilename = L"" FC_CLASSIC_GAME_NAME ".original.dll";

HMODULE loader_module{};
std::once_flag original_once;
GameWinMainFn original_entry{};
fc::loaders::StartupController startup;

// Loads the adjacent original game DLL and retains its entry point; any failure terminates startup.
void initialize_original() {
    const auto directory = fc::loaders::loader_directory(loader_module);
    if (!directory) {
        fc::loaders::fail_startup(loader_module, false, kLoaderName, "Load original game DLL",
                                  "Could not determine the directory containing the " FC_CLASSIC_GAME_NAME " loader",
                                  directory.error().windows_error);
    }

    // Keep the original game DLL loaded for every forwarded GameWinMain call.
    const auto path = *directory / kOriginalFilename;
    const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        fc::loaders::fail_startup(loader_module, false, kLoaderName, "Load original game DLL",
                                  FC_CLASSIC_GAME_NAME ".original.dll could not be loaded", GetLastError());
    }

    original_entry = reinterpret_cast<GameWinMainFn>(GetProcAddress(module, "GameWinMain"));
    if (original_entry == nullptr) {
        fc::loaders::fail_startup(loader_module, false, kLoaderName, "Load original game DLL",
                                  FC_CLASSIC_GAME_NAME ".original.dll does not export GameWinMain", ERROR_PROC_NOT_FOUND);
    }
}

} // namespace

// Call outside loader lock; starts Fusion Cutter and returns the original game's result for all seven launcher arguments.
// A failure to load the original game DLL or start the runtime terminates the process.
extern "C" std::uint32_t GameWinMain(std::uint64_t argument1, std::uint64_t argument2, std::uint64_t argument3, std::uint64_t argument4,
                                     std::uint8_t *argument5, std::uint32_t argument6, std::uint64_t argument7) noexcept {
    // Concurrent launcher calls share the original DLL and wait until its entry point is available.
    try {
        std::call_once(original_once, &initialize_original);
    } catch (...) {
        fc::loaders::fail_startup(loader_module, false, kLoaderName, "Load original game DLL",
                                  "Loading " FC_CLASSIC_GAME_NAME ".original.dll failed unexpectedly", ERROR_GEN_FAILURE);
    }

    // The original game DLL must be loaded before Fusion Cutter identifies the game's images.
    startup.start(loader_module, false, kLoaderName);
    return original_entry(argument1, argument2, argument3, argument4, argument5, argument6, argument7);
}

// Save the proxy's module handle for directory lookup during GameWinMain.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH) {
        loader_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
