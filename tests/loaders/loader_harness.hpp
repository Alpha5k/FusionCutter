#pragma once

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#define FC_TEST_REQUIRE(expression)                                                                                    \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

namespace fusioncutter::tests::loader_harness {

class Sandbox {
  public:
    Sandbox(std::wstring_view name, std::wstring_view artifact_directory_name) {
        std::array<wchar_t, 32'768> root{};
        const auto length = GetTempPathW(static_cast<DWORD>(root.size()), root.data());
        REQUIRE(length > 0);
        REQUIRE(length < root.size());

        path_ = std::filesystem::path(root.data()) /
                (L"FusionCutter-" + std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()));
        artifact_directory_ = path_ / artifact_directory_name;
        working_directory_ = path_ / L"working";
        REQUIRE(std::filesystem::create_directory(path_));
        REQUIRE(std::filesystem::create_directory(artifact_directory_));
        REQUIRE(std::filesystem::create_directory(working_directory_));
    }

    ~Sandbox() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    [[nodiscard]] const std::filesystem::path& artifact_directory() const noexcept {
        return artifact_directory_;
    }

    [[nodiscard]] const std::filesystem::path& working_directory() const noexcept {
        return working_directory_;
    }

  private:
    std::filesystem::path path_;
    std::filesystem::path artifact_directory_;
    std::filesystem::path working_directory_;
};

[[nodiscard]] inline std::filesystem::path environment_path(const wchar_t* name) {
    std::array<wchar_t, 32'768> value{};
    const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) {
        return {};
    }
    return {std::wstring_view{value.data(), length}};
}

inline void copy_artifact(const std::filesystem::path& source, const std::filesystem::path& destination) {
    REQUIRE(!source.empty());
    REQUIRE(std::filesystem::copy_file(source, destination));
}

[[nodiscard]] inline DWORD run_probe(const Sandbox& directory, const wchar_t* probe_environment,
                                     std::string_view mode) {
    const auto probe_directory = directory.artifact_directory().wstring();
    REQUIRE(SetEnvironmentVariableW(probe_environment, probe_directory.c_str()));

    std::array<wchar_t, 32'768> executable_buffer{};
    const auto executable_length =
        GetModuleFileNameW(nullptr, executable_buffer.data(), static_cast<DWORD>(executable_buffer.size()));
    REQUIRE(executable_length > 0);
    REQUIRE(executable_length < executable_buffer.size());
    const std::filesystem::path executable{std::wstring_view{executable_buffer.data(), executable_length}};

    std::wstring command = L"\"" + executable.wstring() + L"\" --probe ";
    command.append(mode.begin(), mode.end());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        directory.working_directory().c_str(), &startup, &process);
    SetEnvironmentVariableW(probe_environment, nullptr);
    REQUIRE(created);

    const auto wait = WaitForSingleObject(process.hProcess, 30'000);
    if (wait == WAIT_TIMEOUT) {
        static_cast<void>(TerminateProcess(process.hProcess, ERROR_TIMEOUT));
        static_cast<void>(WaitForSingleObject(process.hProcess, 5'000));
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

[[nodiscard]] inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace fusioncutter::tests::loader_harness
