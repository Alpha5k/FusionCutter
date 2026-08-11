#include <FusionCutter/LoaderApi.h>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#define DIRECTINPUT_VERSION 0x0800
#define INITGUID
#include <Windows.h>
#include <dinput.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using GetdfDIJoystickFn = LPCDIDATAFORMAT(WINAPI*)();
using GetInitializeCountFn = std::uint32_t(FC_CALL*)();
using CopyInitializeArgsFn = BOOL(FC_CALL*)(FC_InitializeArgs*);
using GetHostEventCountFn = std::uint32_t(FC_CALL*)();
using GetLastHostEventLevelFn = FC_HostEventLevel(FC_CALL*)();
using GetLastHostEventFn = const char*(FC_CALL*)();

#define FC_TEST_REQUIRE(expression)                                                                                    \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::array<wchar_t, 32'768> root{};
        const auto length = GetTempPathW(static_cast<DWORD>(root.size()), root.data());
        REQUIRE(length > 0);
        REQUIRE(length < root.size());

        path_ =
            std::filesystem::path(root.data()) / (L"FusionCutter-dinput8-" + std::to_wstring(GetCurrentProcessId()) +
                                                  L"-" + std::to_wstring(GetTickCount64()));
        REQUIRE(std::filesystem::create_directory(path_));
        REQUIRE(std::filesystem::create_directory(proxy_directory()));
        REQUIRE(std::filesystem::create_directory(working_directory()));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] std::filesystem::path proxy_directory() const {
        return path_ / L"proxy";
    }

    [[nodiscard]] std::filesystem::path working_directory() const {
        return path_ / L"working";
    }

  private:
    std::filesystem::path path_;
};

enum class Setup {
    System,
    Chain,
    InvalidChain,
    AmbiguousChain,
    WorkingDirectoryCore,
};

[[nodiscard]] std::filesystem::path environment_path(const wchar_t* name) {
    std::array<wchar_t, 32'768> value{};
    const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) {
        return {};
    }
    return {std::wstring_view{value.data(), length}};
}

[[nodiscard]] std::filesystem::path executable_path() {
    std::array<wchar_t, 32'768> value{};
    const auto length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    REQUIRE(length > 0);
    REQUIRE(length < value.size());
    return {std::wstring_view{value.data(), length}};
}

void copy_artifact(const std::filesystem::path& source, const std::filesystem::path& destination) {
    REQUIRE(!source.empty());
    REQUIRE(std::filesystem::copy_file(source, destination));
}

void prepare_probe(const TemporaryDirectory& directory, Setup setup) {
    const auto proxy_directory = directory.proxy_directory();
    copy_artifact(environment_path(L"FC_DINPUT8_TEST_DLL"), proxy_directory / L"dinput8.dll");

    const bool adjacent_core = setup != Setup::WorkingDirectoryCore;
    if (adjacent_core) {
        copy_artifact(environment_path(L"FC_DINPUT8_CORE_STUB"), proxy_directory / L"FusionCutter.dll");
    } else {
        copy_artifact(environment_path(L"FC_DINPUT8_CORE_STUB"), directory.working_directory() / L"FusionCutter.dll");
    }

    if (setup == Setup::Chain || setup == Setup::WorkingDirectoryCore) {
        copy_artifact(environment_path(L"FC_DINPUT8_CHAIN_STUB"), proxy_directory / L"dinput8_test.dll");
    } else if (setup == Setup::InvalidChain) {
        std::ofstream invalid(proxy_directory / L"dinput8_invalid.dll", std::ios::binary);
        REQUIRE(invalid);
        invalid << "not a Windows DLL";
    } else if (setup == Setup::AmbiguousChain) {
        const auto chain = environment_path(L"FC_DINPUT8_CHAIN_STUB");
        copy_artifact(chain, proxy_directory / L"dinput8_first.dll");
        copy_artifact(chain, proxy_directory / L"dinput8_second.dll");
    }
}

[[nodiscard]] DWORD run_probe(std::string_view mode, Setup setup) {
    TemporaryDirectory directory;
    prepare_probe(directory, setup);

    const auto probe_directory = directory.proxy_directory().wstring();
    REQUIRE(SetEnvironmentVariableW(L"FC_DINPUT8_PROBE_DIRECTORY", probe_directory.c_str()));

    const auto executable = executable_path();
    std::wstring command = L"\"" + executable.wstring() + L"\" --probe ";
    command.append(mode.begin(), mode.end());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        directory.working_directory().c_str(), &startup, &process);
    SetEnvironmentVariableW(L"FC_DINPUT8_PROBE_DIRECTORY", nullptr);
    REQUIRE(created);

    const auto wait = WaitForSingleObject(process.hProcess, 30'000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5'000);
    }

    DWORD exit_code = STILL_ACTIVE;
    static_cast<void>(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const std::string mode_text{mode};
    CAPTURE(mode_text, wait, exit_code);
    REQUIRE(wait == WAIT_OBJECT_0);
    return exit_code;
}

template <typename Function> [[nodiscard]] Function export_function(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

struct StubApi {
    GetInitializeCountFn initialize_count;
    CopyInitializeArgsFn copy_arguments;
    GetHostEventCountFn host_event_count;
    GetLastHostEventLevelFn last_event_level;
    GetLastHostEventFn last_event;
};

[[nodiscard]] StubApi stub_api(HMODULE module) noexcept {
    return {
        export_function<GetInitializeCountFn>(module, "FC_Test_GetInitializeCount"),
        export_function<CopyInitializeArgsFn>(module, "FC_Test_CopyInitializeArgs"),
        export_function<GetHostEventCountFn>(module, "FC_Test_GetHostEventCount"),
        export_function<GetLastHostEventLevelFn>(module, "FC_Test_GetLastHostEventLevel"),
        export_function<GetLastHostEventFn>(module, "FC_Test_GetLastHostEvent"),
    };
}

[[nodiscard]] int verify_stub(FC_DirectInputChainOutcome expected_outcome, std::uint32_t expected_matches,
                              std::string_view expected_proxy, FC_HostEventLevel expected_event = 0) {
    const auto module = GetModuleHandleW(L"FusionCutter.dll");
    FC_TEST_REQUIRE(module != nullptr);
    const auto api = stub_api(module);
    FC_TEST_REQUIRE(api.initialize_count != nullptr && api.copy_arguments != nullptr &&
                    api.host_event_count != nullptr && api.last_event_level != nullptr && api.last_event != nullptr);
    FC_TEST_REQUIRE(api.initialize_count() == 1);

    FC_InitializeArgs arguments{};
    FC_TEST_REQUIRE(api.copy_arguments(&arguments));
    FC_TEST_REQUIRE(arguments.struct_size == sizeof(arguments));
    FC_TEST_REQUIRE(arguments.host_role == FC_HOST_ROLE_CLIENT);
    FC_TEST_REQUIRE(arguments.loader_startup.struct_size == sizeof(arguments.loader_startup));
    FC_TEST_REQUIRE(arguments.loader_startup.loader_kind == FC_LOADER_KIND_DINPUT8);
    FC_TEST_REQUIRE(arguments.loader_startup.direct_input_chain_outcome == expected_outcome);
    FC_TEST_REQUIRE(arguments.loader_startup.wildcard_match_count == expected_matches);
    FC_TEST_REQUIRE(std::string_view(arguments.loader_startup.selected_proxy_basename) == expected_proxy);

    if (expected_event == 0) {
        FC_TEST_REQUIRE(api.host_event_count() == 0);
    } else {
        FC_TEST_REQUIRE(api.host_event_count() == 1);
        FC_TEST_REQUIRE(api.last_event_level() == expected_event);
        FC_TEST_REQUIRE(std::string_view(api.last_event()).contains("DirectInput"));
    }
    return 0;
}

[[nodiscard]] int call_system_direct_input(DirectInput8CreateFn create) {
    IDirectInput8A* direct_input{};
    const auto result = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                               reinterpret_cast<void**>(&direct_input), nullptr);
    FC_TEST_REQUIRE(SUCCEEDED(result));
    FC_TEST_REQUIRE(direct_input != nullptr);
    direct_input->Release();
    return 0;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] int run_child(std::string_view mode) {
    try {
        if (mode == "chain_fatal") {
            FC_TEST_REQUIRE(SetEnvironmentVariableA("FC_STUB_INIT_RESULT", "Fatal"));
        } else if (mode == "incompatible") {
            FC_TEST_REQUIRE(SetEnvironmentVariableA("FC_STUB_QUERY_MODE", "Unsupported"));
        } else if (mode == "invalid_api") {
            FC_TEST_REQUIRE(SetEnvironmentVariableA("FC_STUB_QUERY_MODE", "InvalidTable"));
        }

        const auto directory = environment_path(L"FC_DINPUT8_PROBE_DIRECTORY");
        FC_TEST_REQUIRE(!directory.empty());
        const auto proxy = LoadLibraryExW((directory / L"dinput8.dll").c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        FC_TEST_REQUIRE(proxy != nullptr);

        constexpr std::array exports{
            "DirectInput8Create", "DllCanUnloadNow",     "DllGetClassObject",
            "DllRegisterServer",  "DllUnregisterServer", "GetdfDIJoystick",
        };
        for (std::size_t index = 0; index < exports.size(); ++index) {
            const auto named_export = GetProcAddress(proxy, exports[index]);
            const auto ordinal_export = GetProcAddress(proxy, MAKEINTRESOURCEA(index + 1));
            FC_TEST_REQUIRE(named_export != nullptr && ordinal_export == named_export);
        }

        const auto create = export_function<DirectInput8CreateFn>(proxy, "DirectInput8Create");
        const auto get_joystick = export_function<GetdfDIJoystickFn>(proxy, "GetdfDIJoystick");
        FC_TEST_REQUIRE(create != nullptr && get_joystick != nullptr);
        FC_TEST_REQUIRE(get_joystick() != nullptr);
        FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") == nullptr);

        const bool chain_forwards_create =
            mode == "chain_fatal" || mode == "missing" || mode == "incompatible" || mode == "invalid_api";
        if (chain_forwards_create) {
            FC_TEST_REQUIRE(create(nullptr, 0, IID_IDirectInput8A, nullptr, nullptr) == S_FALSE);
            FC_TEST_REQUIRE(create(nullptr, 0, IID_IDirectInput8A, nullptr, nullptr) == S_FALSE);
        } else {
            FC_TEST_REQUIRE(call_system_direct_input(create) == 0);
        }

        if (mode == "system") {
            FC_TEST_REQUIRE(call_system_direct_input(create) == 0);
            return verify_stub(FC_DIRECT_INPUT_CHAIN_SYSTEM_ONLY, 0, {});
        }
        if (mode == "chain_fatal") {
            return verify_stub(FC_DIRECT_INPUT_CHAIN_LOADED, 1, "dinput8_test.dll", FC_HOST_EVENT_INFO);
        }
        if (mode == "invalid") {
            return verify_stub(FC_DIRECT_INPUT_CHAIN_INVALID, 1, {}, FC_HOST_EVENT_ERROR);
        }
        if (mode == "ambiguous") {
            return verify_stub(FC_DIRECT_INPUT_CHAIN_AMBIGUOUS, 2, {}, FC_HOST_EVENT_ERROR);
        }

        const auto fallback = read_file(directory / L"FusionCutter.txt");
        FC_TEST_REQUIRE(fallback.contains("Status: Fatal"));
        if (mode == "incompatible") {
            FC_TEST_REQUIRE(fallback.contains("does not support this loader ABI"));
            return 0;
        }
        if (mode == "invalid_api") {
            FC_TEST_REQUIRE(fallback.contains("invalid loader API"));
            return 0;
        }
        FC_TEST_REQUIRE(mode == "missing");
        FC_TEST_REQUIRE(fallback.contains("FusionCutter.dll could not be loaded"));
        FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") == nullptr);

        const auto working_core =
            LoadLibraryExW((std::filesystem::current_path() / L"FusionCutter.dll").c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        FC_TEST_REQUIRE(working_core != nullptr);
        const auto api = stub_api(working_core);
        FC_TEST_REQUIRE(api.initialize_count != nullptr);
        FC_TEST_REQUIRE(api.initialize_count() == 0);
        return 0;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

} // namespace

TEST_CASE("DirectInput loader forwards the complete system surface and starts the client core once",
          "[loaders][dinput8]") {
    CHECK(run_probe("system", Setup::System) == ERROR_SUCCESS);
}

TEST_CASE("DirectInput loader accepts one chain, preserves fallback exports, and forwards after client fatal",
          "[loaders][dinput8]") {
    CHECK(run_probe("chain_fatal", Setup::Chain) == ERROR_SUCCESS);
}

TEST_CASE("DirectInput loader rejects invalid and ambiguous chains without losing system forwarding",
          "[loaders][dinput8]") {
    CHECK(run_probe("invalid", Setup::InvalidChain) == ERROR_SUCCESS);
    CHECK(run_probe("ambiguous", Setup::AmbiguousChain) == ERROR_SUCCESS);
}

TEST_CASE("DirectInput loader uses only an adjacent compatible core and writes fallback status", "[loaders][dinput8]") {
    CHECK(run_probe("missing", Setup::WorkingDirectoryCore) == ERROR_SUCCESS);
    CHECK(run_probe("incompatible", Setup::Chain) == ERROR_SUCCESS);
    CHECK(run_probe("invalid_api", Setup::Chain) == ERROR_SUCCESS);
}

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "--probe") {
        return run_child(argv[2]);
    }
    return Catch::Session().run(argc, argv);
}
