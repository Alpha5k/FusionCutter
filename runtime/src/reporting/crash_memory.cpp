#include "crash_report.hpp"

#include <limits>

namespace fc::runtime::crash_detail {

thread_local bool reporter_read_active{};

namespace {

// Tests whether advancing a byte address would exceed the address type's range.
constexpr bool add_overflows(std::uintptr_t value, std::size_t increment) noexcept {
    return increment > std::numeric_limits<std::uintptr_t>::max() - value;
}

// Accepts Windows page protections that permit reads, excluding guard and inaccessible pages.
bool readable_protection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    switch (protection & 0xffU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

} // namespace

std::size_t readable_extent(std::uintptr_t address, std::size_t maximum) noexcept {
    std::size_t total{};
    // Accumulate adjacent readable regions without wrapping the requested address or a region's end.
    while (total < maximum && !add_overflows(address, total)) {
        const auto cursor = address + total;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void *>(cursor), &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
            !readable_protection(memory.Protect)) {
            break;
        }

        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (cursor < region_base || add_overflows(region_base, memory.RegionSize)) {
            break;
        }
        const auto region_end = region_base + memory.RegionSize;
        if (cursor >= region_end) {
            break;
        }
        total += std::min(maximum - total, static_cast<std::size_t>(region_end - cursor));
    }

    return total;
}

namespace {

// Copies bytes under SEH, suppressing capture of its own faults and restoring any enclosing read state.
// Returns false on a caught SEH exception; destination bytes already copied remain changed.
bool guarded_copy_seh(void *destination, const void *source, std::size_t size) noexcept {
    const auto previous = reporter_read_active;
    reporter_read_active = true;
    bool copied{};
    // The outer finally restores suppression even when SEH unwinds out of the copy.
    __try {
        __try {
            std::memcpy(destination, source, size);
            copied = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    } __finally {
        reporter_read_active = previous;
    }

    return copied;
}

} // namespace

bool guarded_copy(void *destination, const void *source, std::size_t size) noexcept {
    if (size == 0) {
        return true;
    }
    // Mapping inspection avoids known unreadable ranges; SEH handles changes between inspection and copying.
    if (readable_extent(reinterpret_cast<std::uintptr_t>(source), size) != size) {
        return false;
    }

    return guarded_copy_seh(destination, source, size);
}

#if defined(_M_X64)
namespace {

// Advances an x64 context under SEH; a fault returns false and may leave the context partly updated.
bool unwind_once_seh(CONTEXT &context) noexcept {
    __try {
        DWORD64 image_base{};
        auto *function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // Windows treats an address without unwind metadata as a leaf: pop its return address.
            std::uint64_t return_address{};
            if (!guarded_copy(&return_address, reinterpret_cast<const void *>(context.Rsp), sizeof(return_address))) {
                return false;
            }
            context.Rsp += sizeof(return_address);
            context.Rip = return_address;
            return true;
        }

        void *handler_data{};
        DWORD64 establisher_frame{};
        // Use Windows unwind metadata to recover the caller without invoking an exception handler.
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher_frame, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool unwind_once(CONTEXT &context) noexcept {
    // Suppress reporter-generated faults throughout metadata lookup and unwinding, including nested reads.
    const auto previous = reporter_read_active;
    reporter_read_active = true;
    bool unwound{};
    __try {
        unwound = unwind_once_seh(context);
    } __finally {
        reporter_read_active = previous;
    }

    return unwound;
}

#endif

} // namespace fc::runtime::crash_detail
