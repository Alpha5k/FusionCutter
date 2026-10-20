#include "native_memory.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>
#include <system_error>

namespace fc::patching {

namespace {

// Records a write span with uniform page protection so changed pages can be restored in reverse order.
struct ProtectionSlice {
    void *address{};
    std::size_t size{};
    // Saved Windows protection flags restored after the write attempt.
    DWORD original{};
    // Tracks only slices whose writable transition succeeded.
    bool changed{};
};

// Formats a saved Windows error, or the current last error, with the failed operation's name.
std::string windows_error(std::string_view operation, DWORD error = GetLastError()) {
    return std::format("{} failed (Windows error {}: {})", operation, error,
                       std::error_code{static_cast<int>(error), std::system_category()}.message());
}

// Checks the base Windows permission for execution, ignoring protection modifiers.
bool executable(DWORD protection) noexcept {
    const auto base = protection & 0xffU;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

// Splits a native write into accessible, committed regions with uniform Windows protection.
// The caller supplies a nonempty range whose end does not overflow.
std::expected<std::vector<ProtectionSlice>, NativeWriteFailure> query_slices(std::uintptr_t address, std::size_t byte_size) {
    std::vector<ProtectionSlice> result;
    auto current = address;
    const auto end = address + byte_size;

    // Reject inaccessible regions before changing any page protection.
    while (current < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void *>(current), &memory, sizeof(memory)) != sizeof(memory)) {
            return std::unexpected(NativeWriteFailure{windows_error("VirtualQuery for native write")});
        }
        if (memory.State != MEM_COMMIT || memory.Protect == PAGE_NOACCESS || (memory.Protect & PAGE_GUARD) != 0) {
            return std::unexpected(NativeWriteFailure{"Native write target is not accessible committed memory"});
        }

        const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto region_size = static_cast<std::uintptr_t>(memory.RegionSize);
        const auto region_end = region_begin + region_size;
        const auto slice_end = std::min(end, region_end);
        result.push_back({reinterpret_cast<void *>(current), slice_end - current, memory.Protect});
        current = slice_end;
    }

    return result;
}

// Restores the saved protection flags for every changed region in reverse order.
// Returns the first Windows error if any region could not be restored.
[[nodiscard]] DWORD restore_protection(std::span<ProtectionSlice> slices) noexcept {
    DWORD error = ERROR_SUCCESS;
    for (auto iterator = slices.rbegin(); iterator != slices.rend(); ++iterator) {
        if (!iterator->changed) {
            continue;
        }

        DWORD ignored{};
        if (!VirtualProtect(iterator->address, iterator->size, iterator->original, &ignored)) {
            const auto failure = GetLastError();
            if (error == ERROR_SUCCESS) {
                error = failure;
            }
        } else {
            iterator->changed = false;
        }
    }
    return error;
}

} // namespace

std::expected<void, NativeWriteFailure> write_native_memory(std::uintptr_t address, std::span<const std::byte> bytes) {
    if (address == 0 || bytes.empty() || address > std::numeric_limits<std::uintptr_t>::max() - bytes.size()) {
        return std::unexpected(NativeWriteFailure{"Native write range is invalid"});
    }

    auto slices = query_slices(address, bytes.size());
    if (!slices) {
        return std::unexpected(std::move(slices.error()));
    }

    // Make the complete range writable before exposing any replacement byte.
    for (auto &slice : *slices) {
        const auto writable = executable(slice.original) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        DWORD previous{};
        if (!VirtualProtect(slice.address, slice.size, writable, &previous)) {
            auto message = windows_error("VirtualProtect for native write");
            // Undo earlier permission changes even though replacement bytes have not been written yet.
            const auto restore_error = restore_protection(*slices);
            if (restore_error != ERROR_SUCCESS) {
                message += "; " + windows_error("VirtualProtect to restore native write protection", restore_error);
            }
            return std::unexpected(NativeWriteFailure{message, restore_error != ERROR_SUCCESS});
        }
        slice.original = previous;
        slice.changed = true;
    }

    // Write through the process API; success requires both its success result and the complete byte count.
    // Native code may change live data immediately after the write completes.
    SIZE_T written{};
    const auto write_result =
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), bytes.data(), bytes.size(), &written);
    const auto write_error = write_result == FALSE ? GetLastError() : ERROR_SUCCESS;
    const bool write_succeeded = write_result != FALSE && written == bytes.size();

    // Flush whenever the original range included executable pages, even after a failed or partial write.
    const auto flush_result = !std::ranges::any_of(*slices, [](const auto &slice) {
        return executable(slice.original);
    }) || FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(address), bytes.size());
    const auto flush_error = flush_result == FALSE ? GetLastError() : ERROR_SUCCESS;
    const bool flush_succeeded = flush_result != FALSE;

    // Restore all changed slices before reporting any write or cache-flush failure.
    const auto protection_error = restore_protection(*slices);
    const bool protection_succeeded = protection_error == ERROR_SUCCESS;

    if (write_succeeded && flush_succeeded && protection_succeeded) {
        return {};
    }

    // Report the first failed operation and whether bytes were written or protection could not be restored.
    std::string message;
    if (!write_succeeded) {
        message = write_result == FALSE ? windows_error("WriteProcessMemory", write_error)
                                        : std::format("WriteProcessMemory wrote {} of {} native bytes", written, bytes.size());
    } else if (!flush_succeeded) {
        message = windows_error("FlushInstructionCache", flush_error);
    } else if (!protection_succeeded) {
        message = windows_error("VirtualProtect to restore native write protection", protection_error);
    }
    if (!protection_succeeded && (!write_succeeded || !flush_succeeded)) {
        message += "; " + windows_error("VirtualProtect to restore native write protection", protection_error);
    }
    return std::unexpected(NativeWriteFailure{std::move(message), written != 0 || !protection_succeeded});
}

std::expected<void, std::string> check_native_access(std::uintptr_t address, std::size_t size, bool writable, bool code) {
    if (!address || !size || size > std::numeric_limits<std::uintptr_t>::max() - address) {
        return std::unexpected("Native access range is invalid");
    }

    // Walk every VirtualQuery region so a range crossing a permission boundary is checked completely.
    const auto end = address + size;
    for (auto current = address; current < end;) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void *>(current), &memory, sizeof(memory)) != sizeof(memory)) {
            return std::unexpected(windows_error("VirtualQuery for native access"));
        }
        // Separate base access permissions from modifiers such as PAGE_GUARD.
        const auto protection = memory.Protect & 0xffU;
        const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                              protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
                              protection == PAGE_EXECUTE_WRITECOPY;
        const bool write = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READWRITE ||
                           protection == PAGE_EXECUTE_WRITECOPY;
        if (memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) || !readable || (writable && !write) ||
            (code && !executable(memory.Protect))) {
            return std::unexpected("Native range does not have the required current permissions");
        }

        const auto begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        current = std::min(end, begin + static_cast<std::uintptr_t>(memory.RegionSize));
    }

    return {};
}

std::expected<std::vector<std::byte>, std::string> read_native_memory(std::uintptr_t address, std::size_t byte_size) {
    if (address == 0 || byte_size == 0 || address > std::numeric_limits<std::uintptr_t>::max() - byte_size) {
        return std::unexpected("Native read range is invalid");
    }

    // Read through the process API and require the full extent before returning owned bytes.
    std::vector<std::byte> result(byte_size);
    SIZE_T read{};
    const auto completed =
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), result.data(), result.size(), &read);
    if (!completed) {
        return std::unexpected(windows_error("ReadProcessMemory", GetLastError()));
    }
    if (read != result.size()) {
        return std::unexpected(std::format("ReadProcessMemory read {} of {} native bytes", read, result.size()));
    }

    return result;
}

} // namespace fc::patching
