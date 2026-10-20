#pragma once

#include "status.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fc::reporting {

inline constexpr std::uint64_t kFileBytes = 4U * 1024U * 1024U;

// Appends log records and rotates two backup files; used by the reporting worker.
class LogWriter final {
  public:
    // Prepares paths and the session header; file creation waits for the first write.
    LogWriter(std::filesystem::path directory, const SessionFacts &facts);
    // Closes the current output handle.
    ~LogWriter();
    // Appends the complete line, rotating before exceeding kFileBytes; failure sets code and returns false.
    // Calls must be serialized; a failed file write closes the handle so later writes can retry.
    bool write(std::string_view line, DWORD &code) noexcept;

  private:
    // Writes all bytes to the open file and updates its size; short or failed writes set code and return false.
    bool bytes(std::string_view value, DWORD &code) noexcept;
    // Closes the current output handle.
    void close() noexcept;

    std::filesystem::path path_, first_, second_;
    std::string header_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_{};
};

} // namespace fc::reporting
