#pragma once

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fc::tests {

// Owns a temporary test directory and its cleanup.
class TemporaryDirectory {
  public:
    // Creates a uniquely named directory in the system temporary directory.
    TemporaryDirectory() {
        // Separate processes, runs and concurrent calls receive different names.
        static std::atomic_uint64_t sequence{};
        static const auto run = GetTickCount64();
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-Tests-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(run) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        REQUIRE(std::filesystem::create_directory(path));
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    // Removes the directory and its contents; cleanup errors are ignored.
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

// Reads file bytes without newline conversion; an open failure returns an empty string.
inline std::string read_text(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// Runs the command with optional working directory and creation flags; timeout is in milliseconds.
// A timeout or wait failure requests child termination and fails the test.
inline DWORD run_child(std::wstring command, const wchar_t *directory = nullptr, DWORD timeout = 10000, DWORD creation_flags = 0) {
    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, creation_flags, nullptr, directory, &startup, &process));

    struct CloseProcess {
        HANDLE handle;

        // Closes the process handle, including when a test assertion throws.
        ~CloseProcess() {
            CloseHandle(handle);
        }
    } close{process.hProcess};

    CloseHandle(process.hThread);

    // Request termination after a timeout or wait failure before the assertion aborts the test.
    const auto waited = WaitForSingleObject(process.hProcess, timeout);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 0xee);
    }
    REQUIRE(waited == WAIT_OBJECT_0);

    DWORD result{};
    REQUIRE(GetExitCodeProcess(process.hProcess, &result));
    return result;
}
} // namespace fc::tests
