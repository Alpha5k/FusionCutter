#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fc::patching {

// Describes both possible byte exposure and whether page protection still permits safe transaction recovery.
struct NativeWriteFailure {
    std::string message;
    bool changed{};
    bool contained{true};
};

using NativeWriteFunction = std::expected<void, NativeWriteFailure> (*)(void* context, std::uintptr_t address,
                                                                        std::span<const std::byte> bytes);

// A narrow injected writer permits deterministic rollback testing without exposing mutation control above patching.
struct NativeMemoryWriter {
    void* context{};
    NativeWriteFunction write{};
};

// Returns the production writer that temporarily changes page protection, writes, flushes, restores, and verifies.
[[nodiscard]] NativeMemoryWriter system_memory_writer() noexcept;

// Transaction preimages are copied before the first operation can become visible.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> read_native_memory(std::uintptr_t address,
                                                                                    std::size_t byte_size);

} // namespace fc::patching
