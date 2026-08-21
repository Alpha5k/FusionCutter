#include <FusionCutter/CoreApi.h>

#include <Windows.h>
#include <Unknwn.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

using CountFn = std::uint32_t (*)() noexcept;
using CopyStartupFn = BOOL (*)(FC_InitializeArgs*) noexcept;

// Resolved probe callbacks expose the stub framework DLL's observations without linking to fixture internals.
struct StubState {
    CountFn initialize_count{};
    CountFn update_count{};
    CopyStartupFn copy_startup{};
};

// Test-only export resolution keeps the child driver independent of an import library for the staged framework DLL.
template <class Function> [[nodiscard]] Function resolve(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

// Each scenario stages production binaries under their shipping basenames beside one purpose-built framework DLL.
[[nodiscard]] bool copy_fixture(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    return std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error) &&
           !error;
}

[[nodiscard]] std::filesystem::path scenario_directory(std::string_view scenario) {
    // PID and monotonic tick make every child directory unique without recursively deleting a shared location.
    return std::filesystem::temp_directory_path() /
           ("FusionCutter-loader-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) +
            "-" + std::string{scenario});
}

[[nodiscard]] HMODULE wait_for_module(const wchar_t* name) noexcept {
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        if (const auto module = GetModuleHandleW(name)) {
            return module;
        }
        Sleep(10);
    }
    return nullptr;
}

// Worker-started Rcon initialization and the 50 ms pump are observed through bounded polling, not timing sleeps alone.
[[nodiscard]] bool wait_for_count(CountFn count, std::uint32_t minimum) noexcept {
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        if (count != nullptr && count() >= minimum) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

[[nodiscard]] StubState stub_state(HMODULE core) noexcept {
    return {.initialize_count = resolve<CountFn>(core, "FC_Test_InitializeCount"),
            .update_count = resolve<CountFn>(core, "FC_Test_UpdateCount"),
            .copy_startup = resolve<CopyStartupFn>(core, "FC_Test_CopyStartup")};
}

[[nodiscard]] bool valid_state(const StubState& state) noexcept {
    return state.initialize_count != nullptr && state.update_count != nullptr && state.copy_startup != nullptr;
}

#if defined(_WIN64)

using GameWinMainFn = std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint8_t*,
                                        std::uint32_t, std::uint64_t);

[[nodiscard]] bool invoke_client_entry(const std::filesystem::path& directory) {
    const auto proxy = LoadLibraryW((directory / L"Battlefront2.dll").c_str());
    if (proxy == nullptr) {
        return false;
    }
    const auto entry = resolve<GameWinMainFn>(proxy, "GameWinMain");
    // The Classic proxy's reviewed public boundary names GameWinMain and also preserves its entry at ordinal 1.
    if (entry == nullptr || GetProcAddress(proxy, MAKEINTRESOURCEA(1)) != reinterpret_cast<FARPROC>(entry)) {
        return false;
    }
    // Simultaneous GameWinMain calls prove original loading and common framework startup each use one-time state.
    std::uint32_t first{};
    std::uint32_t second{};
    std::thread left([&] {
        first = entry(1, 2, 3, 4, nullptr, 5, 6);
    });
    std::thread right([&] {
        second = entry(1, 2, 3, 4, nullptr, 5, 6);
    });
    left.join();
    right.join();
    return first == 21 && second == 21;
}

#else

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);

[[nodiscard]] bool invoke_client_entry(const std::filesystem::path& directory) {
    const auto proxy = LoadLibraryW((directory / L"dinput8.dll").c_str());
    if (proxy == nullptr) {
        return false;
    }
    // Every export that mirrors the system DLL must resolve through both the standard name and its observed ordinal.
    constexpr std::array names{"DirectInput8Create", "DllCanUnloadNow",     "DllGetClassObject",
                               "DllRegisterServer",  "DllUnregisterServer", "GetdfDIJoystick"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto named = GetProcAddress(proxy, names[index]);
        if (named == nullptr || GetProcAddress(proxy, MAKEINTRESOURCEA(index + 1)) != named) {
            return false;
        }
    }
    const auto entry = resolve<DirectInput8CreateFn>(proxy, "DirectInput8Create");
    if (entry == nullptr) {
        return false;
    }
    // The adjacent chain accepts both calls while concurrent first use exercises forwarding and framework once guards.
    const GUID ignored{};
    HRESULT first{};
    HRESULT second{};
    std::thread left([&] {
        void* output{};
        first = entry(GetModuleHandleW(nullptr), 0x0800, ignored, &output, nullptr);
    });
    std::thread right([&] {
        void* output{};
        second = entry(GetModuleHandleW(nullptr), 0x0800, ignored, &output, nullptr);
    });
    left.join();
    right.join();
    return first == S_OK && second == S_OK;
}

#endif

[[nodiscard]] int run_client(std::string_view mode) {
    // The selected client proxy, auxiliary target, and stub framework DLL use their deployed filenames.
    const auto directory = scenario_directory(mode);
    std::error_code error;
    if (!std::filesystem::create_directories(directory, error) || error ||
        !copy_fixture(FC_CORE_STUB_PATH, directory / L"FusionCutter.dll") ||
        !copy_fixture(FC_CLIENT_LOADER_PATH, directory / FC_CLIENT_LOADER_NAME)
#if defined(_WIN64)
        || !copy_fixture(FC_CLIENT_AUXILIARY_PATH, directory / L"Battlefront2.original.dll")
#else
        || !copy_fixture(FC_CLIENT_AUXILIARY_PATH, directory / L"dinput8_chain.dll")
#endif
    ) {
        return 10;
    }
    SetEnvironmentVariableA("FC_TEST_CORE_RESULT", std::string{mode}.c_str());
    if (!invoke_client_entry(directory)) {
        return 11;
    }
    // Structured loader facts prove the shipping entry, not test code, supplied the complete initialization tuple.
    const auto core = wait_for_module(L"FusionCutter.dll");
    const auto state = stub_state(core);
    if (core == nullptr || !valid_state(state) || state.initialize_count() != 1) {
        return 12;
    }
    FC_InitializeArgs startup{};
    if (state.copy_startup(&startup) == FALSE || startup.host_role != FC_HOST_ROLE_CLIENT ||
        startup.loader_startup.loader_kind != FC_CLIENT_LOADER_KIND ||
        startup.loader_startup.direct_input_chain != FC_CLIENT_CHAIN_RESULT) {
        return 13;
    }
    // The Unsupported result has no pump; the Completed result must demonstrate recurring Update callbacks.
    if (mode == "unsupported") {
        Sleep(150);
        return state.update_count() == 0 ? 0 : 14;
    }
    return wait_for_count(state.update_count, 2) ? 0 : 15;
}

[[nodiscard]] int run_client_fallback() {
    // Omitting FusionCutter.dll exercises the compatibility report without weakening the real forwarding path.
    const auto directory = scenario_directory("fallback");
    std::error_code error;
    if (!std::filesystem::create_directories(directory, error) || error ||
        !copy_fixture(FC_CLIENT_LOADER_PATH, directory / FC_CLIENT_LOADER_NAME)
#if defined(_WIN64)
        || !copy_fixture(FC_CLIENT_AUXILIARY_PATH, directory / L"Battlefront2.original.dll")
#else
        || !copy_fixture(FC_CLIENT_AUXILIARY_PATH, directory / L"dinput8_chain.dll")
#endif
    ) {
        return 20;
    }
    if (!invoke_client_entry(directory)) {
        return 21;
    }
    std::ifstream input{directory / L"FusionCutter.txt", std::ios::binary};
    const std::string status{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return status.find("Fusion Cutter loader ") != std::string::npos &&
                   status.find("Initialization: Unavailable") != std::string::npos &&
                   status.find("Started: ") != std::string::npos && status.find("Architecture: ") != std::string::npos
               ? 0
               : 22;
}

[[nodiscard]] int run_server(std::string_view mode) {
    // Rcon uses its DllMain bootstrap worker; Classic additionally needs a loaded game image before that worker starts.
    const auto directory = scenario_directory(mode);
    std::error_code error;
    if (!std::filesystem::create_directories(directory, error) || error ||
        !copy_fixture(FC_CORE_STUB_PATH, directory / L"FusionCutter.dll") ||
        !copy_fixture(FC_SERVER_LOADER_PATH, directory / FC_SERVER_LOADER_NAME)) {
        return 30;
    }
#if defined(_WIN64)
    if (!copy_fixture(FC_CLIENT_AUXILIARY_PATH, directory / L"Battlefront2.dll") ||
        LoadLibraryW((directory / L"Battlefront2.dll").c_str()) == nullptr) {
        return 31;
    }
#endif
    SetEnvironmentVariableA("FC_TEST_CORE_RESULT", std::string{mode}.c_str());
    if (LoadLibraryW((directory / FC_SERVER_LOADER_NAME).c_str()) == nullptr) {
        return 32;
    }
    // A Fatal initialization result never reaches these assertions because the helper terminates the child.
    const auto core = wait_for_module(L"FusionCutter.dll");
    const auto state = stub_state(core);
    if (core == nullptr || !valid_state(state) || !wait_for_count(state.initialize_count, 1) ||
        state.initialize_count() != 1) {
        return 33;
    }
    FC_InitializeArgs startup{};
    if (state.copy_startup(&startup) == FALSE || startup.host_role != FC_HOST_ROLE_SERVER ||
        startup.loader_startup.loader_kind != FC_LOADER_KIND_RCONSERVER ||
        startup.loader_startup.direct_input_chain != FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE) {
        return 34;
    }
    return wait_for_count(state.update_count, 2) ? 0 : 35;
}

} // namespace

int main(int count, char** arguments) {
    if (count != 2) {
        return 2;
    }
    // Named scenarios keep process-lifetime loader state isolated while sharing one architecture-specific driver.
    const std::string_view scenario{arguments[1]};
    if (scenario == "completed") {
        return run_client(scenario);
    }
    if (scenario == "unsupported") {
        return run_client(scenario);
    }
    if (scenario == "fallback") {
        return run_client_fallback();
    }
    if (scenario == "server-completed" || scenario == "server-fatal") {
        return run_server(scenario == "server-fatal" ? "fatal" : "completed");
    }
    return 3;
}
