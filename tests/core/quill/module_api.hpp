#pragma once

#include <cstdint>

namespace fusioncutter::tests::quill {

inline constexpr auto kStartName = "FcQuillStart";
inline constexpr auto kLogName = "FcQuillLog";
inline constexpr auto kSaturateName = "FcQuillSaturate";
inline constexpr auto kStopName = "FcQuillStop";

using StartFunction = int(__cdecl*)(const char* log_path, bool slow_backend) noexcept;
using LogFunction = void(__cdecl*)(const char* message) noexcept;
using SaturateFunction = std::uint64_t(__cdecl*)(std::uint32_t message_count) noexcept;
using StopFunction = std::uint64_t(__cdecl*)() noexcept;

} // namespace fusioncutter::tests::quill
