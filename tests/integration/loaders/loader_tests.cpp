#include <FusionCutter/CoreApi.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

// Runs one loader per child because successful pumps and fatal process policy deliberately live for process lifetime.
[[nodiscard]] DWORD run_child(std::string_view scenario) {
    std::string command = "\"" FC_LOADER_CHILD_PATH "\" ";
    command.append(scenario);
    STARTUPINFOA startup{.cb = sizeof(STARTUPINFOA)};
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) !=
            FALSE);
    CloseHandle(process.hThread);
    REQUIRE(WaitForSingleObject(process.hProcess, 10'000) == WAIT_OBJECT_0);
    DWORD exit_code{};
    REQUIRE(GetExitCodeProcess(process.hProcess, &exit_code) != FALSE);
    CloseHandle(process.hProcess);
    return exit_code;
}

} // namespace

TEST_CASE("The architecture client loader negotiates once, starts one pump, and forwards its real entry", "[loaders]") {
    CHECK(run_child("completed") == 0);
}

TEST_CASE("The architecture client loader does not pump an unsupported target", "[loaders]") {
    CHECK(run_child("unsupported") == 0);
}

TEST_CASE("The architecture client loader keeps compatible forwarding when framework API negotiation is unavailable",
          "[loaders]") {
    CHECK(run_child("fallback") == 0);
}

TEST_CASE("The architecture RconServer loader uses the shared server startup and pump", "[loaders]") {
    CHECK(run_child("server-completed") == 0);
}

TEST_CASE("A fatal server initialization terminates with the common loader exit code", "[loaders]") {
    CHECK(run_child("server-fatal") == 0xD1);
}

TEST_CASE("FusionCutter.dll exports one immutable query table for ABI generation 1", "[loaders][core-api]") {
    const auto module = LoadLibraryW(FC_PRODUCTION_CORE_PATH);
    REQUIRE(module != nullptr);
    using Query = const FC_CoreApi*(FC_CALL*)(std::uint32_t) noexcept;
    const auto query = reinterpret_cast<Query>(GetProcAddress(module, "FusionCutter_QueryCore"));
    REQUIRE(query != nullptr);
    CHECK(query(FC_CORE_ABI_GENERATION + 1) == nullptr);
    const auto* first = query(FC_CORE_ABI_GENERATION);
    const auto* second = query(FC_CORE_ABI_GENERATION);
    REQUIRE(first != nullptr);
    CHECK(first == second);
    REQUIRE(first->struct_size >= offsetof(FC_CoreApi, update) + sizeof(first->update));
    REQUIRE(first->initialize != nullptr);
    REQUIRE(first->update != nullptr);

    // The first malformed call fixes a Fatal result; later malformed input and Update callbacks remain inert.
    FC_InitializeArgs malformed{};
    CHECK(first->initialize(&malformed) == FC_INIT_FATAL);
    CHECK(first->initialize(nullptr) == FC_INIT_FATAL);
    first->update();
}
