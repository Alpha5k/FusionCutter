#include "child_support.hpp"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <fstream>

#include <thread>

namespace fc::tests::loaders {

int client(std::string_view scenario) {
    const auto directory = stage();
    if (const auto failure = prepare_client(directory, scenario)) {
        return failure;
    }

    auto chain = directory / L"dinput8_chain.dll";
    const bool fallback =
        scenario == "zero" || scenario == "multiple" || scenario == "missing-primary" || scenario == "copied" || scenario == "recursive";
    if (scenario != "zero" && scenario != "secondary") {
        if (scenario == "copied") {
            // A byte-identical loader copy must be rejected before it can become a forwarding destination.
            if (!copy_fixture(FC_CLIENT_LOADER_PATH, chain)) {
                return 16;
            }
        } else if (!copy_fixture(scenario == "missing-primary" ? FC_NO_EXPORT_PATH
                                 : scenario == "full"          ? FC_FULL_CHAIN_PATH
                                                               : FC_CLIENT_AUXILIARY_PATH,
                                 chain)) {
            return 17;
        }
        if (scenario == "multiple") {
            // A second matching filename makes candidate selection ambiguous even though the first DLL is valid.
            std::ofstream file(directory / L"dinput8_invalid.dll");
            file << "invalid";
        }
    }

    const bool game_ext = scenario.starts_with("gameext");
    const bool game_ext_loaded = game_ext && scenario != "gameext-disabled" && scenario != "gameext-invalid" &&
                                 scenario != "gameext-missing-init" && scenario != "gameext-failed-init";
    if (game_ext) {
        if (scenario == "gameext-invalid") {
            std::ofstream(directory / L"BF2GameExt.dll") << "invalid";
        } else if (!copy_fixture(scenario == "gameext-missing-init" ? FC_NO_EXPORT_PATH : FC_CLIENT_AUXILIARY_PATH,
                                 directory / L"BF2GameExt.dll")) {
            return 26;
        }
        std::ofstream ini(directory / L"BF2GameExt.ini");
        if (scenario != "gameext") {
            ini << "[General]\nEnabled=" << (scenario == "gameext-disabled" ? 0 : 1) << '\n';
        }
        ini << "[Fixture]\nValue=42\n";
    }
    SetEnvironmentVariableA("BF2GAMEEXT_PROXY", scenario == "gameext-marker" ? "existing" : nullptr);
    SetEnvironmentVariableA("FC_TEST_CHAIN_MODE", scenario == "recursive" ? "recursive" : game_ext ? scenario.data() : nullptr);
    if (scenario == "gameext-preloaded" && !LoadLibraryW((directory / L"BF2GameExt.dll").c_str())) {
        return 27;
    }
    const auto proxy = LoadLibraryW((directory / FC_CLIENT_LOADER_NAME).c_str());
    if (!proxy) {
        return 18;
    }

    // A secondary export initializes forwarding; Fusion Cutter starts later on DirectInput8Create.
    const auto joystick = resolve<LPCDIDATAFORMAT(WINAPI *)()>(proxy, "GetdfDIJoystick");
    if (!joystick || !joystick() || GetModuleHandleW(L"FusionCutter.dll") ||
        (scenario != "gameext-preloaded" && GetModuleHandleW(L"BF2GameExt.dll"))) {
        return 20;
    }
    if (scenario == "secondary" || scenario == "gameext-secondary") {
        return 0;
    }
    if (scenario == "full" && (joystick()->dwFlags != 0x4321 || resolve<HRESULT(WINAPI *)()>(proxy, "DllCanUnloadNow")() != 0x12345)) {
        return 21;
    }

    using Entry = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, IUnknown *);
    const auto entry = resolve<Entry>(proxy, "DirectInput8Create");
    const GUID id{};
    HRESULT first{}, second{};
    void *first_output{};
    void *second_output{};

    // Race the first calls to check shared initialization, except when testing permanent recursion fallback.
    std::thread left([&] {
        first = entry(GetModuleHandleW(nullptr), 0x0800, id, &first_output, nullptr);
    });
    if (scenario == "recursive") {
        // Finish the recursive call first so the second call must use the remembered system fallback.
        left.join();
    }
    std::thread right([&] {
        second = entry(GetModuleHandleW(nullptr), 0x0800, id, &second_output, nullptr);
    });
    if (left.joinable()) {
        left.join();
    }
    right.join();

    if (!startup_captured()) {
        return 22;
    }

    // Successful GameExt startup retains one initialized module; disabled or failed requests remain optional.
    const auto game_ext_module = GetModuleHandleW(L"BF2GameExt.dll");
    if (static_cast<bool>(game_ext_module) != game_ext_loaded) {
        return 28;
    }
    if (game_ext_module) {
        const auto count = resolve<std::uint32_t (*)()>(game_ext_module, "FC_Test_GameExtCount");
        if (!count || count() != (scenario == "gameext-preloaded" ? 0u : 1u)) {
            return 29;
        }
    }
    char marker[32]{};
    GetEnvironmentVariableA("BF2GAMEEXT_PROXY", marker, sizeof(marker));
    if (std::string_view{marker} != (scenario == "gameext-marker" ? "existing" : "")) {
        return 30;
    }

    // Compare fallback results with the real system DLL using the same arguments.
    if (fallback) {
        std::array<wchar_t, 32768> path{};
        GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
        const auto system = LoadLibraryW((std::filesystem::path{path.data()} / L"dinput8.dll").c_str());
        void *output{};
        const auto expected = resolve<Entry>(system, "DirectInput8Create")(GetModuleHandleW(nullptr), 0x0800, id, &output, nullptr);
        if (first != expected || second != expected) {
            return 23;
        }
        if (scenario == "recursive") {
            const auto module = GetModuleHandleW(L"dinput8_chain.dll");
            const auto count = resolve<std::uint32_t (*)()>(module, "FC_Test_ChainCount");
            if (!count || count() != 1) {
                return 24;
            }
        }
        return 0;
    }
    return first == S_OK && second == S_OK && first_output == reinterpret_cast<void *>(0x1234) && second_output == first_output ? 0 : 25;
}

} // namespace fc::tests::loaders
