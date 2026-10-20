#pragma once

#include <fc/abi/reporting.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace fc::runtime {

// Owns a runtime diagnostic's source label, message and optional Windows error code.
struct Diagnostic {
    // Identifies the declaration, path, or runtime component that failed.
    std::string owner;
    std::string message;
    // Optional Windows error code associated with this diagnostic; zero when none applies.
    std::uint32_t windows_error{};
    // Recoverable configuration warnings override the default failure severity.
    FC_LogLevel level = FC_LOG_ERROR;
};

// Copies a callback's borrowed text and Windows code into the caller-owned Diagnostic supplied as context.
inline void receive_diagnostic(void *context, FC_Text message, std::uint32_t code) {
    auto &error = *static_cast<Diagnostic *>(context);
    error.message.assign(message.data, message.size);
    error.windows_error = code;
}

// Formats a diagnostic with its Windows error code and system message when present.
std::string diagnostic_text(const Diagnostic &);

// Copies a native filesystem path as UTF-8 for diagnostic and log text.
inline std::string path_text(const std::filesystem::path &path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

} // namespace fc::runtime
