#include "native_memory.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

namespace fc::patching {
namespace {

// Records a write span with uniform page protection so changed pages can be restored in reverse order.
struct ProtectionSlice {
    void* address{};
    std::size_t size{};
    DWORD original{};
    bool changed{};
};

[[nodiscard]] std::string windows_error(std::string_view operation, DWORD error = GetLastError()) {
    return std::string{operation} + " failed with Windows error " + std::to_string(error);
}

[[nodiscard]] bool executable(DWORD protection) noexcept {
    const auto base = protection & 0xffU;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

// Splits a native range at VirtualQuery region boundaries because one patch may cross protection regimes.
[[nodiscard]] std::expected<std::vector<ProtectionSlice>, NativeWriteFailure> query_slices(std::uintptr_t address,
                                                                                           std::size_t byte_size) {
    std::vector<ProtectionSlice> result;
    auto current = address;
    const auto end = address + byte_size;
    // Reject inaccessible or malformed regions before changing any page protection.
    while (current < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &memory, sizeof(memory)) != sizeof(memory)) {
            return std::unexpected(NativeWriteFailure{windows_error("VirtualQuery native write")});
        }
        if (memory.State != MEM_COMMIT || memory.Protect == PAGE_NOACCESS || (memory.Protect & PAGE_GUARD) != 0) {
            return std::unexpected(NativeWriteFailure{"Native write target is not accessible committed memory"});
        }
        const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto region_size = static_cast<std::uintptr_t>(memory.RegionSize);
        if (region_begin > current || region_size > std::numeric_limits<std::uintptr_t>::max() - region_begin) {
            return std::unexpected(NativeWriteFailure{"Native write protection region is malformed"});
        }
        const auto region_end = region_begin + region_size;
        if (region_end <= current) {
            return std::unexpected(NativeWriteFailure{"Native write protection region does not advance"});
        }
        const auto slice_end = std::min(end, region_end);
        result.push_back({reinterpret_cast<void*>(current), slice_end - current, memory.Protect});
        current = slice_end;
    }
    return result;
}

// Restores every page already changed; a failure means the framework can no longer claim coherent protection state.
[[nodiscard]] bool restore_protection(std::span<ProtectionSlice> slices) noexcept {
    bool restored = true;
    for (auto iterator = slices.rbegin(); iterator != slices.rend(); ++iterator) {
        if (!iterator->changed) {
            continue;
        }
        DWORD ignored{};
        if (!VirtualProtect(iterator->address, iterator->size, iterator->original, &ignored)) {
            restored = false;
        } else {
            iterator->changed = false;
        }
    }
    return restored;
}

[[nodiscard]] std::expected<void, NativeWriteFailure> system_write(void*, std::uintptr_t address,
                                                                   std::span<const std::byte> bytes) {
    if (address == 0 || bytes.empty() || address > std::numeric_limits<std::uintptr_t>::max() - bytes.size()) {
        return std::unexpected(NativeWriteFailure{"Native write range is invalid"});
    }
    auto slices = query_slices(address, bytes.size());
    if (!slices) {
        return std::unexpected(std::move(slices.error()));
    }

    // Make the complete range writable before exposing any replacement byte.
    for (auto& slice : *slices) {
        const auto writable = executable(slice.original) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        DWORD previous{};
        if (!VirtualProtect(slice.address, slice.size, writable, &previous)) {
            const auto message = windows_error("VirtualProtect native write");
            const bool coherent = restore_protection(*slices);
            return std::unexpected(NativeWriteFailure{message, false, coherent});
        }
        slice.changed = true;
    }

    // Publish, flush, restore, and verify as one operation so callers can distinguish exposure from containment.
    SIZE_T written{};
    const auto write_result =
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.data(), bytes.size(), &written);
    const auto write_error = write_result == FALSE ? GetLastError() : ERROR_SUCCESS;
    const bool write_succeeded = write_result != FALSE && written == bytes.size();
    const auto flush_result =
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.size());
    const auto flush_error = flush_result == FALSE ? GetLastError() : ERROR_SUCCESS;
    const bool flush_succeeded = flush_result != FALSE;
    const bool protection_succeeded = restore_protection(*slices);

    std::vector<std::byte> observed(bytes.size());
    SIZE_T read{};
    const bool verified = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                            observed.data(), observed.size(), &read) != FALSE &&
                          read == observed.size() && std::equal(observed.begin(), observed.end(), bytes.begin());
    if (write_succeeded && flush_succeeded && protection_succeeded && verified) {
        return {};
    }

    // Report the earliest failed boundary while retaining byte exposure and page protection coherence independently.
    std::string message;
    if (!write_succeeded) {
        message = write_result == FALSE ? windows_error("WriteProcessMemory", write_error)
                                        : "WriteProcessMemory wrote an incomplete native range";
    } else if (!flush_succeeded) {
        message = windows_error("FlushInstructionCache", flush_error);
    } else if (!protection_succeeded) {
        message = "Native write could not restore memory protection";
    } else {
        message = "Native write verification failed";
    }
    return std::unexpected(NativeWriteFailure{std::move(message), written != 0, protection_succeeded});
}

} // namespace

NativeMemoryWriter system_memory_writer() noexcept {
    return {.write = &system_write};
}

std::expected<std::vector<std::byte>, std::string> read_native_memory(std::uintptr_t address, std::size_t byte_size) {
    if (address == 0 || byte_size == 0 || address > std::numeric_limits<std::uintptr_t>::max() - byte_size) {
        return std::unexpected("Native read range is invalid");
    }
    std::vector<std::byte> result(byte_size);
    SIZE_T read{};
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), result.data(), result.size(),
                           &read) ||
        read != result.size()) {
        return std::unexpected(windows_error("ReadProcessMemory"));
    }
    return result;
}

} // namespace fc::patching
