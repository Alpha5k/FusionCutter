#include "original_stub.hpp"

#include <FusionCutter/LoaderApi.h>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using GameWinMainFn = std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint8_t*,
                                        std::uint32_t, std::uint64_t);
using GetCallCountFn = std::uint32_t (*)();
using CopyArgumentsFn = BOOL (*)(fusioncutter::tests::GameWinMainArguments*);
using GetInitializeCountFn = std::uint32_t(FC_CALL*)();
using CopyInitializeArgsFn = BOOL(FC_CALL*)(FC_InitializeArgs*);
using WasRequiredModuleLoadedFn = BOOL(FC_CALL*)();

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

        path_ = std::filesystem::path(root.data()) /
                (L"FusionCutter-battlefront2-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()));
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
    Complete,
    WorkingDirectoryCore,
    MissingOriginal,
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
    copy_artifact(environment_path(L"FC_BATTLEFRONT2_TEST_DLL"), proxy_directory / L"Battlefront2.dll");

    if (setup != Setup::MissingOriginal) {
        copy_artifact(environment_path(L"FC_BATTLEFRONT2_ORIGINAL_STUB"),
                      proxy_directory / L"Battlefront2.original.dll");
    }

    const auto core = environment_path(L"FC_BATTLEFRONT2_CORE_STUB");
    if (setup == Setup::WorkingDirectoryCore) {
        copy_artifact(core, directory.working_directory() / L"FusionCutter.dll");
    } else {
        copy_artifact(core, proxy_directory / L"FusionCutter.dll");
    }
}

[[nodiscard]] DWORD run_probe(std::string_view mode, Setup setup) {
    TemporaryDirectory directory;
    prepare_probe(directory, setup);

    const auto probe_directory = directory.proxy_directory().wstring();
    REQUIRE(SetEnvironmentVariableW(L"FC_BATTLEFRONT2_PROBE_DIRECTORY", probe_directory.c_str()));

    const auto executable = executable_path();
    std::wstring command = L"\"" + executable.wstring() + L"\" --probe ";
    command.append(mode.begin(), mode.end());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto working_directory = directory.working_directory().wstring();
    const auto created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        working_directory.c_str(), &startup, &process);
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

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] int verify_core(HMODULE module) {
    const auto initialize_count = export_function<GetInitializeCountFn>(module, "FC_Test_GetInitializeCount");
    const auto copy_arguments = export_function<CopyInitializeArgsFn>(module, "FC_Test_CopyInitializeArgs");
    const auto required_module_loaded =
        export_function<WasRequiredModuleLoadedFn>(module, "FC_Test_WasRequiredModuleLoaded");
    FC_TEST_REQUIRE(initialize_count != nullptr && copy_arguments != nullptr && required_module_loaded != nullptr);
    FC_TEST_REQUIRE(initialize_count() == 1);
    FC_TEST_REQUIRE(required_module_loaded());

    FC_InitializeArgs arguments{};
    FC_TEST_REQUIRE(copy_arguments(&arguments));
    FC_TEST_REQUIRE(arguments.host_role == FC_HOST_ROLE_CLIENT);
    FC_TEST_REQUIRE(arguments.loader_startup.loader_kind == FC_LOADER_KIND_BATTLEFRONT2);
    FC_TEST_REQUIRE(arguments.loader_startup.direct_input_chain_outcome == FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE);
    return 0;
}

[[nodiscard]] int verify_forwarding(HMODULE original, const fusioncutter::tests::GameWinMainArguments& expected) {
    const auto call_count = export_function<GetCallCountFn>(original, "FC_Test_GetGameWinMainCallCount");
    const auto copy_arguments = export_function<CopyArgumentsFn>(original, "FC_Test_CopyGameWinMainArguments");
    FC_TEST_REQUIRE(call_count != nullptr && copy_arguments != nullptr);
    FC_TEST_REQUIRE(call_count() == 2);

    fusioncutter::tests::GameWinMainArguments actual{};
    FC_TEST_REQUIRE(copy_arguments(&actual));
    FC_TEST_REQUIRE(actual.argument1 == expected.argument1);
    FC_TEST_REQUIRE(actual.argument2 == expected.argument2);
    FC_TEST_REQUIRE(actual.argument3 == expected.argument3);
    FC_TEST_REQUIRE(actual.argument4 == expected.argument4);
    FC_TEST_REQUIRE(actual.argument5 == expected.argument5);
    FC_TEST_REQUIRE(actual.argument6 == expected.argument6);
    FC_TEST_REQUIRE(actual.argument7 == expected.argument7);
    return 0;
}

[[nodiscard]] int run_child(std::string_view mode) {
    try {
        if (mode == "fatal") {
            FC_TEST_REQUIRE(SetEnvironmentVariableA("FC_STUB_INIT_RESULT", "Fatal"));
        } else if (mode == "incompatible") {
            FC_TEST_REQUIRE(SetEnvironmentVariableA("FC_STUB_QUERY_MODE", "Unsupported"));
        }
        FC_TEST_REQUIRE(SetEnvironmentVariableW(L"FC_STUB_REQUIRED_MODULE", L"Battlefront2.original.dll"));

        const auto directory = environment_path(L"FC_BATTLEFRONT2_PROBE_DIRECTORY");
        FC_TEST_REQUIRE(!directory.empty());
        const auto proxy = LoadLibraryExW((directory / L"Battlefront2.dll").c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        FC_TEST_REQUIRE(proxy != nullptr);

        const auto named_entry = GetProcAddress(proxy, "GameWinMain");
        const auto ordinal_entry = GetProcAddress(proxy, MAKEINTRESOURCEA(1));
        FC_TEST_REQUIRE(named_entry != nullptr && ordinal_entry == named_entry);
        const auto game_win_main = reinterpret_cast<GameWinMainFn>(named_entry);

        FC_TEST_REQUIRE(GetModuleHandleW(L"Battlefront2.original.dll") == nullptr);
        FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") == nullptr);

        std::uint8_t pointer_argument{};
        const fusioncutter::tests::GameWinMainArguments arguments{
            0x1111'1111'1111'1111, 0x2222'2222'2222'2222, 0x3333'3333'3333'3333, 0x4444'4444'4444'4444,
            &pointer_argument,     0x6666'6666,           0x7777'7777'7777'7777,
        };

        const auto first_result =
            game_win_main(arguments.argument1, arguments.argument2, arguments.argument3, arguments.argument4,
                          arguments.argument5, arguments.argument6, arguments.argument7);
        if (mode == "missing_original") {
            FC_TEST_REQUIRE(first_result != ERROR_SUCCESS);
            FC_TEST_REQUIRE(first_result != fusioncutter::tests::kGameWinMainResult);
            FC_TEST_REQUIRE(GetModuleHandleW(L"Battlefront2.original.dll") == nullptr);
            FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") == nullptr);
            const auto fallback = read_file(directory / L"FusionCutter.txt");
            FC_TEST_REQUIRE(fallback.contains("Battlefront2.original.dll could not be loaded"));
            FC_TEST_REQUIRE(fallback.contains("Restore a compatible Battlefront2.original.dll"));

            const auto core = LoadLibraryExW((directory / L"FusionCutter.dll").c_str(), nullptr,
                                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            FC_TEST_REQUIRE(core != nullptr);
            const auto initialize_count = export_function<GetInitializeCountFn>(core, "FC_Test_GetInitializeCount");
            FC_TEST_REQUIRE(initialize_count != nullptr && initialize_count() == 0);
            return 0;
        }

        FC_TEST_REQUIRE(first_result == fusioncutter::tests::kGameWinMainResult);
        FC_TEST_REQUIRE(game_win_main(arguments.argument1, arguments.argument2, arguments.argument3,
                                      arguments.argument4, arguments.argument5, arguments.argument6,
                                      arguments.argument7) == fusioncutter::tests::kGameWinMainResult);

        const auto original = GetModuleHandleW(L"Battlefront2.original.dll");
        FC_TEST_REQUIRE(original != nullptr);
        FC_TEST_REQUIRE(verify_forwarding(original, arguments) == 0);

        if (mode == "success" || mode == "fatal") {
            const auto core = GetModuleHandleW(L"FusionCutter.dll");
            FC_TEST_REQUIRE(core != nullptr);
            FC_TEST_REQUIRE(verify_core(core) == 0);
            FC_TEST_REQUIRE(FreeLibrary(proxy));
            FC_TEST_REQUIRE(GetModuleHandleW(L"Battlefront2.original.dll") != nullptr);
            FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") != nullptr);
            return 0;
        }

        FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") == nullptr);
        const auto fallback = read_file(directory / L"FusionCutter.txt");
        FC_TEST_REQUIRE(fallback.contains("Status: Fatal"));
        if (mode == "incompatible") {
            FC_TEST_REQUIRE(fallback.contains("does not support this loader ABI"));
            return 0;
        }

        FC_TEST_REQUIRE(mode == "missing_core");
        FC_TEST_REQUIRE(fallback.contains("FusionCutter.dll could not be loaded"));
        const auto working_core =
            LoadLibraryExW((std::filesystem::current_path() / L"FusionCutter.dll").c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        FC_TEST_REQUIRE(working_core != nullptr);
        const auto initialize_count = export_function<GetInitializeCountFn>(working_core, "FC_Test_GetInitializeCount");
        FC_TEST_REQUIRE(initialize_count != nullptr && initialize_count() == 0);
        return 0;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

} // namespace

TEST_CASE("Battlefront2 loader forwards every GameWinMain argument and return value after starting the core once",
          "[loaders][battlefront2]") {
    CHECK(run_probe("success", Setup::Complete) == ERROR_SUCCESS);
}

TEST_CASE("Battlefront2 loader continues through missing, incompatible, and client-fatal core outcomes",
          "[loaders][battlefront2]") {
    CHECK(run_probe("fatal", Setup::Complete) == ERROR_SUCCESS);
    CHECK(run_probe("missing_core", Setup::WorkingDirectoryCore) == ERROR_SUCCESS);
    CHECK(run_probe("incompatible", Setup::Complete) == ERROR_SUCCESS);
}

TEST_CASE("Battlefront2 loader reports a missing original without starting the core", "[loaders][battlefront2]") {
    CHECK(run_probe("missing_original", Setup::MissingOriginal) == ERROR_SUCCESS);
}

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "--probe") {
        return run_child(argv[2]);
    }
    return Catch::Session().run(argc, argv);
}
