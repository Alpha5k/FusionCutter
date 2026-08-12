#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace {

class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(std::wstring_view name) {
        std::array<wchar_t, 32'768> temporary_root{};
        const auto length = GetTempPathW(static_cast<DWORD>(temporary_root.size()), temporary_root.data());
        REQUIRE(length > 0);
        REQUIRE(length < temporary_root.size());

        path_ = std::filesystem::path(temporary_root.data()) /
                (L"FusionCutter-quill-" + std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
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

[[nodiscard]] std::filesystem::path environment_path(const wchar_t* name) {
    std::array<wchar_t, 32'768> value{};
    const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    REQUIRE(length > 0);
    REQUIRE(length < value.size());
    return {value.data()};
}

[[nodiscard]] ChildResult run_child(std::wstring_view mode, const TemporaryDirectory& directory) {
    const auto executable = environment_path(L"FC_QUILL_PROBE");
    const auto module = environment_path(L"FC_QUILL_MODULE");
    std::wstring command = L"\"" + executable.wstring() + L"\" " + std::wstring(mode) + L" \"" + module.wstring() +
                           L"\" \"" + directory.path().wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           executable.parent_path().c_str(), &startup, &process));

    const auto wait_result = WaitForSingleObject(process.hProcess, 8'000);
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

[[nodiscard]] std::string read_log(const TemporaryDirectory& directory) {
    std::ifstream input(directory.path() / L"quill.log", std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void require_successful_probe(std::wstring_view mode) {
    TemporaryDirectory directory(mode);
    const auto child = run_child(mode, directory);
    REQUIRE(child.wait_result == WAIT_OBJECT_0);
    REQUIRE(child.exit_code == ERROR_SUCCESS);
    CHECK(read_log(directory).contains("Fusion Cutter Quill acceptance marker"));
}

} // namespace

TEST_CASE("Quill does not hang or fault during explicit DLL unload", "[core][quill]") {
    require_successful_probe(L"explicit-unload");
}

TEST_CASE("Quill bounded queue drops without blocking its producer", "[core][quill]") {
    require_successful_probe(L"saturation");
}
