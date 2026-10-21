#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <filesystem>
#include <string>
#include <utility>
#include <string_view>
#include "../test_support.hpp"

namespace {

// Each case runs in a child process so retained loader state and fatal exits remain isolated.
// Returns the child's exit code and optionally copies its fallback status report.
DWORD run_child(std::string_view scenario, std::string *fallback = nullptr) {
    fc::tests::TemporaryDirectory directory;
    REQUIRE(SetEnvironmentVariableW(L"FC_TEST_LOADER_DIRECTORY", directory.path.c_str()));

    std::wstring command = L"\"" + std::filesystem::path{FC_LOADER_CHILD_PATH}.wstring() + L"\" ";
    command.append(scenario.begin(), scenario.end());
    const auto result = fc::tests::run_child(std::move(command));

    SetEnvironmentVariableW(L"FC_TEST_LOADER_DIRECTORY", nullptr);
    if (fallback) {
        // Read the report after the child exits and before the temporary directory is removed.
        *fallback = fc::tests::read_text(directory.path / L"FusionCutter.txt");
    }
    return result;
}
} // namespace

TEST_CASE("The client loader starts once and forwards its complete native entry", "[loaders]") {
    CHECK(run_child("completed") == 0);
#if defined(_WIN64)
    CHECK(run_child("bf1-completed") == 0);
#endif
}

TEST_CASE("Client runtime loading and initialization failures terminate startup", "[loaders]") {
    for (const auto scenario : {"missing-runtime", "missing-export", "failed"}) {
        INFO(scenario);
        std::string fallback;
        CHECK(run_child(scenario, &fallback) == 0xD1);
        CHECK(fallback.find("Failed") != std::string::npos);
        CHECK(fallback.find("Reason:") != std::string::npos);
        if (std::string_view{scenario} == "failed") {
            CHECK(fallback.find("Fixture initialization rejected the request") != std::string::npos);
        }
    }
}

TEST_CASE("The production bootstrap rejects ABI/options before initialization", "[loaders]") {
    CHECK(run_child("runtime-boundary") == 0);
}
#if defined(_WIN64)
TEST_CASE("Classic requires its original module before starting the runtime", "[loaders]") {
    CHECK(run_child("missing-original") == 0xD1);
    CHECK(run_child("bf1-missing-original") == 0xD1);
}
#else
TEST_CASE("Secondary DirectInput exports forward without starting the runtime", "[loaders]") {
    CHECK(run_child("secondary") == 0);
}

TEST_CASE("DirectInput retains optional exports and system fallback across candidate failures", "[loaders]") {
    for (const auto mode : {"zero", "full", "multiple", "missing-primary", "copied", "recursive"}) {
        INFO(mode);
        CHECK(run_child(mode) == 0);
    }
}

TEST_CASE("DirectInput initializes GameExt when the proxy loads and preserves its INI", "[loaders]") {
    for (const auto mode : {"gameext", "gameext-secondary", "gameext-disabled", "gameext-marker", "gameext-preloaded", "gameext-invalid",
                            "gameext-missing-init", "gameext-failed-init"}) {
        INFO(mode);
        CHECK(run_child(mode) == 0);
    }
}
#endif

#if defined(FC_TEST_DEDICATED)
TEST_CASE("Combined dedicated startup uses its own production runtime and discovers external plugins", "[loaders][runtime]") {
    CHECK(run_child("combined") == 0);
#if defined(_WIN64)
    CHECK(run_child("combined-bf1") == 0);
#endif
}

TEST_CASE("A second combined runtime copy fails through the loader startup policy", "[loaders][runtime]") {
    CHECK(run_child("combined-copy") == 0xD1);
}
#endif
