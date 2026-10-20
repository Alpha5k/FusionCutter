#include "child_support.hpp"
#include <fstream>
#include <iterator>

namespace fc::tests::loaders {

int combined(std::string_view scenario) {
    const auto directory = stage();
    std::filesystem::create_directory(directory / L"plugins");
    if (!copy_fixture(FC_SERVER_LOADER_PATH, directory / FC_SERVER_LOADER_NAME) ||
        !copy_fixture(FC_STARTUP_PROBE_PATH, directory / L"plugins" / L"startup_probe.dll")) {
        return 40;
    }

    const auto caller = GetCurrentThreadId();
    const auto module = LoadLibraryW((directory / FC_SERVER_LOADER_NAME).c_str());
    if (!module) {
        return 41;
    }
#if defined(_WIN64)
    // Verify that Classic startup waits for either game's DLL name before discovering plugins.
    Sleep(100);
    if (GetModuleHandleW(L"startup_probe.dll")) {
        return 42;
    }
    const auto *game_name = scenario == "combined-bf1" ? L"Battlefront1.dll" : L"Battlefront2.dll";
    if (!copy_fixture(FC_CLIENT_AUXILIARY_PATH, directory / game_name) || !LoadLibraryW((directory / game_name).c_str())) {
        return 43;
    }
#endif

    // Plugin discovery records the runtime's initialization thread, which must differ from this loader caller.
    HMODULE probe{};
    std::uint32_t plugin_thread{};
    for (unsigned attempt = 0; attempt < 300 && !probe; ++attempt) {
        probe = GetModuleHandleW(L"startup_probe.dll");
        const auto thread = resolve<std::uint32_t (*)()>(probe, "FC_Test_PluginThread");
        plugin_thread = thread ? thread() : 0;
        if (!plugin_thread) {
            probe = nullptr;
            Sleep(10);
        }
    }

    if (!probe || GetModuleHandleW(L"FusionCutter.dll")) {
        return 44;
    }
    if (!plugin_thread || plugin_thread == caller) {
        return 45;
    }

    using Start = std::uint32_t(FC_CDECL *)(std::uint32_t, const FC_StartOptions *, FC_ErrorSink);
    const auto start = resolve<Start>(module, "FC_Start");
    if (!start) {
        return 46;
    }

    // Repeating the same launch mode succeeds; requesting a different mode after startup is rejected.
    const FC_StartOptions dedicated{sizeof(dedicated), 1};
    StartError error;
    if (start(FC_ABI_VERSION, &dedicated, {&error, &StartError::receive}) != 1) {
        return 47;
    }

    const FC_StartOptions client_options{sizeof(client_options), 0};
    if (start(FC_ABI_VERSION, &client_options, {&error, &StartError::receive}) != 0 || error.message.empty()) {
        return 48;
    }

    // Loading a second runtime copy should terminate this child through the loader's failure policy.
    if (scenario == "combined-copy") {
        if (!copy_fixture(FC_SERVER_LOADER_PATH, directory / L"RconServer_copy.dll") ||
            !LoadLibraryW((directory / L"RconServer_copy.dll").c_str())) {
            return 49;
        }
        Sleep(3000);
        return 50;
    }

    // Wait for the reporting worker to publish the status collected during startup.
    for (unsigned attempt = 0; attempt != 300; ++attempt) {
        std::ifstream status{directory / L"FusionCutter.txt", std::ios::binary};
        const std::string report{std::istreambuf_iterator<char>{status}, std::istreambuf_iterator<char>{}};
        if (report.find("startup_probe") != std::string::npos) {
            return 0;
        }
        Sleep(10);
    }
    return 51;
}

} // namespace fc::tests::loaders
