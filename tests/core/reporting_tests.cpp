#include "bounded_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::array<wchar_t, 32'768> temporary_root{};
        const auto length = GetTempPathW(static_cast<DWORD>(temporary_root.size()), temporary_root.data());
        REQUIRE(length > 0);
        REQUIRE(length < temporary_root.size());

        path_ = std::filesystem::path(temporary_root.data()) /
                (L"FusionCutter-reporting-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()));
        REQUIRE(std::filesystem::create_directory(path_));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

struct ChildResult {
    DWORD wait_result;
    DWORD exit_code;
};

[[nodiscard]] std::filesystem::path reporting_probe_path() {
    std::array<wchar_t, 32'768> value{};
    const auto length = GetEnvironmentVariableW(L"FC_REPORTING_PROBE", value.data(), static_cast<DWORD>(value.size()));
    REQUIRE(length > 0);
    REQUIRE(length < value.size());
    return {value.data()};
}

[[nodiscard]] ChildResult run_child(const std::filesystem::path& executable, std::wstring_view mode) {
    std::wstring command = L"\"" + executable.wstring() + L"\" " + std::wstring(mode);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           executable.parent_path().c_str(), &startup, &process));

    const auto wait_result = WaitForSingleObject(process.hProcess, 30'000);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5'000);
    }

    DWORD exit_code = STILL_ACTIVE;
    static_cast<void>(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return {wait_result, exit_code};
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::size_t occurrence_count(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

[[nodiscard]] std::filesystem::path copy_probe(const TemporaryDirectory& directory) {
    const auto destination = directory.path() / L"reporting_probe.exe";
    REQUIRE(std::filesystem::copy_file(reporting_probe_path(), destination));
    return destination;
}

} // namespace

TEST_CASE("Bounded crash text records truncation without overflowing", "[core][reporting]") {
    constexpr std::string_view kMarker = "[truncated]";
    std::array<char, 32> buffer{};
    fusioncutter::reporting::detail::BoundedWriter output(buffer, kMarker);

    output.append("A deliberately oversized diagnostic field");
    const auto result = output.finish();

    REQUIRE(result.size() == buffer.size());
    CHECK(std::string_view(result.data(), result.size()).ends_with(kMarker));
}

TEST_CASE("Crash reporter suppresses expected faults and bounds same-session reports", "[core][reporting]") {
    TemporaryDirectory directory;
    const auto executable = copy_probe(directory);
    const auto report_path = directory.path() / L"FusionCutter-Crash.log";
    {
        std::ofstream stale(report_path, std::ios::binary);
        stale << "stale report from an earlier process";
    }

    const auto child = run_child(executable, L"sequence");
    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    REQUIRE(child.exit_code == ERROR_SUCCESS);

    const auto report = read_file(report_path);
    CHECK(report.size() <= 4 * 64 * 1024);
    CHECK_FALSE(report.contains("stale report"));
    CHECK(occurrence_count(report, "Fusion Cutter first-chance exception report") == 4);
    CHECK(occurrence_count(report, "(ACCESS_VIOLATION)") == 4);
    CHECK_FALSE(report.contains("INTEGER_DIVIDE_BY_ZERO"));
    CHECK(report.contains("ReportNumber: 1"));
    CHECK(report.contains("ReportNumber: 4"));
    CHECK_FALSE(report.contains("ReportNumber: 5"));
    CHECK(report.contains("SelectedDirectInputProxy: reporting-test-proxy.dll"));
    CHECK(report.contains("Target: SteamRetail"));
    CHECK(report.contains("TargetFingerprint: reporting-test-target"));
    CHECK(report.contains("CorePhase: PatchCommit"));
    CHECK(report.contains("CurrentPatch: test_patch"));
    CHECK(report.contains("  - installed_test_patch"));
    CHECK(report.contains("[Hook owner=test_patch]"));
    CHECK(report.contains("Registers\r\n"));
    CHECK(report.contains("StackFrames\r\n"));
    CHECK(report.contains("InstructionBytes\r\n"));
    CHECK(report.contains("StackWindow\r\n"));
    CHECK(report.contains("ImplicatedModules\r\n"));
}

TEST_CASE("Unhandled native exception terminates the child after writing one report", "[core][reporting]") {
    TemporaryDirectory directory;
    const auto executable = copy_probe(directory);
    const auto child = run_child(executable, L"fatal");

    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    CHECK(child.exit_code == EXCEPTION_ACCESS_VIOLATION);

    const auto report = read_file(directory.path() / L"FusionCutter-Crash.log");
    CHECK(occurrence_count(report, "Fusion Cutter first-chance exception report") == 1);
    CHECK(report.contains("This report records an observed exception"));
    CHECK(report.contains("(ACCESS_VIOLATION)"));
    CHECK(report.contains("EndOfReport"));
}

TEST_CASE("Ordinary reporting filters before lazy output and renders relevant status", "[core][reporting]") {
    TemporaryDirectory directory;
    const auto executable = copy_probe(directory);
    const auto child = run_child(executable, L"ordinary");
    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    REQUIRE(child.exit_code == ERROR_SUCCESS);

    const auto log = read_file(directory.path() / L"FusionCutter.log");
    CHECK(log.contains("Fusion Cutter development"));
    CHECK(log.contains("[AimAssist] expected instruction bytes did not match"));
    CHECK(log.contains("Operation: Install input hook"));
    CHECK_FALSE(log.contains("filtered informational record"));

    const auto status = read_file(directory.path() / L"FusionCutter.txt");
    CHECK(status.contains("Fusion Cutter development\r\nStatus: Completed"));
    CHECK(status.contains("Target: Steam (Client,"));
    CHECK(status.contains("Configuration: FusionCutter.ini"));
    CHECK(status.contains("Log: FusionCutter.log"));
    CHECK(status.contains("Installed patches:\r\n  DLC Mission Limit"));
    CHECK(status.contains("Failed patches:\r\n  Aim Assist"));
    CHECK(status.contains("Operation: Install input hook"));
    CHECK(status.contains("Reason: expected instruction bytes did not match"));
    CHECK(status.contains("Direct Transport:\r\n  State: Idle\r\n  Route: Direct"));
    CHECK_FALSE(status.contains("Disabled Patch"));
    CHECK_FALSE(status.contains("Wrong Target"));
}

TEST_CASE("Logging Off still publishes status without creating a log", "[core][reporting]") {
    TemporaryDirectory directory;
    const auto executable = copy_probe(directory);
    const auto child = run_child(executable, L"off");
    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    REQUIRE(child.exit_code == ERROR_SUCCESS);

    CHECK_FALSE(std::filesystem::exists(directory.path() / L"FusionCutter.log"));
    CHECK(read_file(directory.path() / L"FusionCutter.txt").contains("Log: Logging disabled"));
}

TEST_CASE("Ordinary log rotation retains the previous bounded generation", "[core][reporting]") {
    TemporaryDirectory directory;
    const auto executable = copy_probe(directory);
    const auto log_path = directory.path() / L"FusionCutter.log";
    {
        std::ofstream stale(log_path, std::ios::binary);
        REQUIRE(stale);
        const std::string block(64 * 1024, 'X');
        for (int index = 0; index < 64; ++index) {
            stale.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
        REQUIRE(stale);
    }

    const auto child = run_child(executable, L"rotation");
    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    REQUIRE(child.exit_code == ERROR_SUCCESS);

    CHECK(std::filesystem::file_size(directory.path() / L"FusionCutter.1.log") == 4 * 1024 * 1024);
    CHECK(read_file(log_path).contains("rotation marker"));
    CHECK_FALSE(std::filesystem::exists(directory.path() / L"FusionCutter.3.log"));
}

TEST_CASE("Status refresh is bounded to changed snapshots", "[core][reporting]") {
    TemporaryDirectory directory;
    const auto executable = copy_probe(directory);
    const auto child = run_child(executable, L"live-status");
    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    REQUIRE(child.exit_code == ERROR_SUCCESS);
}

TEST_CASE("Live status fields truncate safely and reject excess entries", "[core][reporting]") {
    TemporaryDirectory directory;
    const auto executable = copy_probe(directory);
    const auto child = run_child(executable, L"bounded-status");
    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    REQUIRE(child.exit_code == ERROR_SUCCESS);

    const auto status = read_file(directory.path() / L"FusionCutter.txt");
    CHECK(status.contains("Bounded Feature:"));
    CHECK(status.contains("Long: " + std::string(192, 'X')));
    CHECK_FALSE(status.contains(std::string(193, 'X')));
    CHECK(status.contains("Field11: Value"));
    CHECK_FALSE(status.contains("Overflow"));
    CHECK(status.size() < 64 * 1024);
}

TEST_CASE("Status output failure remains nonfatal", "[core][reporting]") {
    TemporaryDirectory directory;
    const auto executable = copy_probe(directory);
    REQUIRE(std::filesystem::create_directory(directory.path() / L"FusionCutter.txt"));

    const auto child = run_child(executable, L"status-output-failure");
    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    REQUIRE(child.exit_code == ERROR_SUCCESS);
}
