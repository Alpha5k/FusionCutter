#include "log_writer.hpp"

#include <fc/abi.h>

#include <format>

namespace fc::reporting {

namespace {

// Replaces the destination with the source; a missing source also succeeds.
bool move_if_present(const std::filesystem::path &from, const std::filesystem::path &to) noexcept {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    const auto code = GetLastError();
    return code == ERROR_FILE_NOT_FOUND;
}

} // namespace

LogWriter::LogWriter(std::filesystem::path directory, const SessionFacts &facts)
    : path_(directory / "FusionCutter.log"), first_(directory / "FusionCutter.1.log"), second_(directory / "FusionCutter.2.log") {
    header_ = std::format("Fusion Cutter {}\r\nStarted: {:%Y-%m-%d %H:%M:%S} UTC; PID {}; Role: {}; Executable: {}\r\n", FC_VERSION_STRING,
                          facts.started, GetCurrentProcessId(), facts.role == FC_ROLE_SERVER ? "Server" : "Client",
                          facts.executable_basename.empty() ? "Unknown" : facts.executable_basename);
}

LogWriter::~LogWriter() {
    close();
}

bool LogWriter::write(std::string_view line, DWORD &code) noexcept {
    bool needs_header = file_ == INVALID_HANDLE_VALUE;
    const auto header_bytes = needs_header ? header_.size() : 0;
    if (line.size() > kFileBytes - header_.size()) {
        code = ERROR_BUFFER_OVERFLOW;
        return false;
    }

    // Opening on demand also retries a file that was closed after an earlier write failure.
    if (file_ == INVALID_HANDLE_VALUE) {
        file_ = CreateFileW(path_.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            code = GetLastError();
            return false;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size)) {
            code = GetLastError();
            close();
            return false;
        }
        size_ = static_cast<std::uint64_t>(size.QuadPart);
    }

    // Move the older backup first so rotating the current file does not overwrite it.
    if (size_ > kFileBytes - line.size() - header_bytes) {
        close();
        if (!move_if_present(first_, second_) || !move_if_present(path_, first_)) {
            code = GetLastError();
            return false;
        }
        file_ = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            code = GetLastError();
            return false;
        }
        size_ = 0;
        needs_header = true;
    }

    if (needs_header && !bytes(header_, code)) {
        close();
        return false;
    }
    if (!bytes(line, code)) {
        close();
        return false;
    }
    return true;
}

bool LogWriter::bytes(std::string_view value, DWORD &code) noexcept {
    DWORD written{};
    if (!WriteFile(file_, value.data(), static_cast<DWORD>(value.size()), &written, nullptr) || written != value.size()) {
        code = GetLastError();
        if (!code) {
            code = ERROR_WRITE_FAULT;
        }
        return false;
    }
    size_ += value.size();
    return true;
}

void LogWriter::close() noexcept {
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
    }

    file_ = INVALID_HANDLE_VALUE;
}

} // namespace fc::reporting
