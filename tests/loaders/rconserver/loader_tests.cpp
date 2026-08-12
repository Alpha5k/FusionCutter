#include "../loader_harness.hpp"

#include <FusionCutter/LoaderApi.h>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

using GetInitializeCountFn = std::uint32_t(FC_CALL*)();
using GetUpdateCountFn = std::uint32_t(FC_CALL*)();
using CopyInitializeArgsFn = BOOL(FC_CALL*)(FC_InitializeArgs*);
using WasRequiredModuleLoadedFn = BOOL(FC_CALL*)();

constexpr DWORD kFatalExitCode = 0xD1;

using fusioncutter::tests::loader_harness::copy_artifact;
using fusioncutter::tests::loader_harness::environment_path;
using fusioncutter::tests::loader_harness::export_function;
using fusioncutter::tests::loader_harness::read_file;

enum class Setup {
    Complete,
    WorkingDirectoryCore,
};

void prepare_probe(const fusioncutter::tests::loader_harness::Sandbox& directory, Setup setup) {
    const auto loader_directory = directory.artifact_directory();
    const auto loader = environment_path(L"FC_RCONSERVER_TEST_DLL");
    const auto core = environment_path(L"FC_RCONSERVER_CORE_STUB");
    copy_artifact(loader, loader_directory / loader.filename());
    copy_artifact(core, loader_directory / L"Battlefront2.dll");

    if (setup == Setup::Complete) {
        copy_artifact(core, loader_directory / L"FusionCutter.dll");
    } else {
        copy_artifact(core, directory.working_directory() / L"FusionCutter.dll");
    }
}

[[nodiscard]] DWORD run_probe(std::string_view mode, Setup setup) {
    fusioncutter::tests::loader_harness::Sandbox directory{L"rconserver", L"loader"};
    prepare_probe(directory, setup);
    return fusioncutter::tests::loader_harness::run_probe(directory, L"FC_RCONSERVER_PROBE_DIRECTORY", mode);
}

template <typename Predicate> [[nodiscard]] bool wait_until(Predicate&& predicate) {
    const auto deadline = GetTickCount64() + 5'000;
    while (GetTickCount64() < deadline) {
        if (predicate()) {
            return true;
        }
        Sleep(10);
    }
    return predicate();
}

struct StubApi {
    GetInitializeCountFn initialize_count;
    GetUpdateCountFn update_count;
    CopyInitializeArgsFn copy_arguments;
    WasRequiredModuleLoadedFn required_module_loaded;
};

[[nodiscard]] StubApi stub_api(HMODULE module) noexcept {
    return {
        export_function<GetInitializeCountFn>(module, "FC_Test_GetInitializeCount"),
        export_function<GetUpdateCountFn>(module, "FC_Test_GetUpdateCount"),
        export_function<CopyInitializeArgsFn>(module, "FC_Test_CopyInitializeArgs"),
        export_function<WasRequiredModuleLoadedFn>(module, "FC_Test_WasRequiredModuleLoaded"),
    };
}

[[nodiscard]] int verify_completed(HMODULE core) {
    const auto api = stub_api(core);
    FC_TEST_REQUIRE(api.initialize_count != nullptr && api.update_count != nullptr && api.copy_arguments != nullptr &&
                    api.required_module_loaded != nullptr);
    FC_TEST_REQUIRE(wait_until([&] {
        return api.initialize_count() == 1 && api.update_count() > 0;
    }));
    FC_TEST_REQUIRE(api.required_module_loaded());

    FC_InitializeArgs arguments{};
    FC_TEST_REQUIRE(api.copy_arguments(&arguments));
    FC_TEST_REQUIRE(arguments.struct_size == sizeof(arguments));
    FC_TEST_REQUIRE(arguments.host_role == FC_HOST_ROLE_SERVER);
    FC_TEST_REQUIRE(arguments.loader_startup.struct_size == sizeof(arguments.loader_startup));
    FC_TEST_REQUIRE(arguments.loader_startup.loader_kind == FC_LOADER_KIND_RCONSERVER);
    FC_TEST_REQUIRE(arguments.loader_startup.direct_input_chain_outcome == FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE);
    return 0;
}

[[nodiscard]] int run_child(std::string_view mode) {
    try {
        if (mode == "fatal") {
            FC_TEST_REQUIRE(SetEnvironmentVariableA("FC_STUB_INIT_RESULT", "Fatal"));
        } else if (mode == "unsupported") {
            FC_TEST_REQUIRE(SetEnvironmentVariableA("FC_STUB_INIT_RESULT", "Unsupported"));
        } else if (mode == "incompatible") {
            FC_TEST_REQUIRE(SetEnvironmentVariableA("FC_STUB_QUERY_MODE", "Unsupported"));
        }
        FC_TEST_REQUIRE(SetEnvironmentVariableW(L"FC_STUB_REQUIRED_MODULE", L"Battlefront2.dll"));

        const auto directory = environment_path(L"FC_RCONSERVER_PROBE_DIRECTORY");
        FC_TEST_REQUIRE(!directory.empty());
#if !defined(_WIN64)
        const auto game = LoadLibraryExW((directory / L"Battlefront2.dll").c_str(), nullptr,
                                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        FC_TEST_REQUIRE(game != nullptr);
#endif

        const auto loader_source = environment_path(L"FC_RCONSERVER_TEST_DLL");
        const auto loader = LoadLibraryExW((directory / loader_source.filename()).c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        FC_TEST_REQUIRE(loader != nullptr);

#if defined(_WIN64)
        Sleep(100);
        FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") == nullptr);
        const auto game = LoadLibraryExW((directory / L"Battlefront2.dll").c_str(), nullptr,
                                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        FC_TEST_REQUIRE(game != nullptr);
#endif

        if (mode == "fatal") {
            Sleep(5'000);
            return ERROR_TIMEOUT;
        }

        if (mode == "success" || mode == "unsupported") {
            HMODULE core{};
            FC_TEST_REQUIRE(wait_until([&] {
                core = GetModuleHandleW(L"FusionCutter.dll");
                return core != nullptr;
            }));
            const auto api = stub_api(core);
            FC_TEST_REQUIRE(api.initialize_count != nullptr && api.update_count != nullptr);
            if (mode == "success") {
                return verify_completed(core);
            }

            FC_TEST_REQUIRE(wait_until([&] {
                return api.initialize_count() == 1;
            }));
            Sleep(100);
            FC_TEST_REQUIRE(api.update_count() == 0);
            return 0;
        }

        const auto status_path = directory / L"FusionCutter-Server.txt";
        FC_TEST_REQUIRE(wait_until([&] {
            return std::filesystem::exists(status_path);
        }));
        const auto fallback = read_file(status_path);
        FC_TEST_REQUIRE(fallback.contains("Status: Fatal"));
        if (mode == "incompatible") {
            FC_TEST_REQUIRE(fallback.contains("does not support this loader ABI"));
            FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") == nullptr);
            return 0;
        }

        FC_TEST_REQUIRE(mode == "missing");
        FC_TEST_REQUIRE(fallback.contains("FusionCutter.dll could not be loaded"));
        FC_TEST_REQUIRE(GetModuleHandleW(L"FusionCutter.dll") == nullptr);
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

TEST_CASE("RconServer loader starts the server core once and pumps completed initialization", "[loaders][rconserver]") {
    CHECK(run_probe("success", Setup::Complete) == ERROR_SUCCESS);
}

TEST_CASE("RconServer loader does not pump an unsupported server", "[loaders][rconserver]") {
    CHECK(run_probe("unsupported", Setup::Complete) == ERROR_SUCCESS);
}

TEST_CASE("RconServer loader terminates the server on fatal initialization", "[loaders][rconserver]") {
    CHECK(run_probe("fatal", Setup::Complete) == kFatalExitCode);
}

TEST_CASE("RconServer loader uses only an adjacent compatible core and writes server fallback status",
          "[loaders][rconserver]") {
    CHECK(run_probe("missing", Setup::WorkingDirectoryCore) == ERROR_SUCCESS);
    CHECK(run_probe("incompatible", Setup::Complete) == ERROR_SUCCESS);
}

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "--probe") {
        return run_child(argv[2]);
    }
    return Catch::Session().run(argc, argv);
}
