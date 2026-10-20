#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fc::patching {

// Checks that every region is committed and readable, plus writable or executable when requested.
// The result describes current permissions; later protection changes can invalidate it.
std::expected<void, std::string> check_native_access(std::uintptr_t address, std::size_t size, bool writable = false, bool code = false);

// Reports a failed native write and whether it exposed bytes or left page protection unrestored.
struct NativeWriteFailure {
    std::string message;
    // True requires retaining resources that native code may now reference.
    bool unsafe{};
};

// Temporarily makes the range writable, checks the byte count, flushes executable changes and restores protection.
// Callers arrange synchronization with native threads; failures can leave changed bytes or protection.
[[nodiscard]] std::expected<void, NativeWriteFailure> write_native_memory(std::uintptr_t address, std::span<const std::byte> bytes);

// Returns exactly byte_size copied bytes, or an error for an invalid range, failed read or short read.
std::expected<std::vector<std::byte>, std::string> read_native_memory(std::uintptr_t address, std::size_t byte_size);

} // namespace fc::patching
