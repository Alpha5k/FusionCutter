#include "memory.hpp"

#include <Windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace fusioncutter::patching_detail {
namespace {

[[nodiscard]] std::string windows_error(std::string_view action) {
    return std::string{action} + " failed with Windows error " + std::to_string(GetLastError());
}

[[nodiscard]] bool is_executable(DWORD protection) noexcept {
    const auto base = protection & 0xFF;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] std::uintptr_t align_down(std::uintptr_t value, std::size_t alignment) noexcept {
    return value - value % alignment;
}

[[nodiscard]] std::uintptr_t align_up(std::uintptr_t value, std::size_t alignment) noexcept {
    const auto remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

[[nodiscard]] bool within_distance(std::uintptr_t left, std::uintptr_t right, std::size_t maximum_distance) noexcept {
    const auto distance = left > right ? left - right : right - left;
    return distance <= maximum_distance;
}

[[nodiscard]] void* try_allocate_at(std::uintptr_t candidate, std::size_t allocation_size, std::size_t alignment,
                                    std::uintptr_t reference, std::size_t maximum_distance) noexcept {
    auto* allocation =
        VirtualAlloc(reinterpret_cast<void*>(candidate), allocation_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (allocation == nullptr) {
        return nullptr;
    }

    const auto aligned = align_up(reinterpret_cast<std::uintptr_t>(allocation), alignment);
    if (within_distance(aligned, reference, maximum_distance)) {
        return allocation;
    }

    VirtualFree(allocation, 0, MEM_RELEASE);
    return nullptr;
}

[[nodiscard]] void* allocate_near(std::uintptr_t reference, std::size_t maximum_distance, std::size_t allocation_size,
                                  std::size_t alignment) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::size_t>(info.dwAllocationGranularity);
    const auto minimum = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    const auto first = align_down(reference, granularity);
    const auto lower = reference > maximum_distance ? std::max(minimum, reference - maximum_distance) : minimum;
    const auto upper = maximum - reference > maximum_distance ? reference + maximum_distance : maximum;

    for (std::size_t distance = 0;;) {
        const auto can_search_below = first >= lower && distance <= first - lower;
        const auto can_search_above = distance <= upper - first;

        if (can_search_below) {
            if (auto* allocation =
                    try_allocate_at(first - distance, allocation_size, alignment, reference, maximum_distance)) {
                return allocation;
            }
        }

        if (distance != 0 && can_search_above) {
            if (auto* allocation =
                    try_allocate_at(first + distance, allocation_size, alignment, reference, maximum_distance)) {
                return allocation;
            }
        }

        if (!can_search_below && !can_search_above) {
            return nullptr;
        }
        if (distance > std::numeric_limits<std::size_t>::max() - granularity) {
            return nullptr;
        }
        distance += granularity;
    }
}

} // namespace

bool readable_memory(std::uintptr_t address, std::size_t size) noexcept {
    if (address == 0 || size == 0 || address > std::numeric_limits<std::uintptr_t>::max() - size) {
        return false;
    }

    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto region_begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return address >= region_begin && size <= region.RegionSize - (address - region_begin);
}

std::expected<void, WriteFailure> write_memory(std::uintptr_t address, std::span<const std::byte> bytes) {
    if (address == 0 || bytes.empty() || address > std::numeric_limits<std::uintptr_t>::max() - bytes.size()) {
        return std::unexpected(WriteFailure{"invalid memory-write range", false});
    }

    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region)) {
        return std::unexpected(WriteFailure{windows_error("VirtualQuery"), false});
    }
    if (region.State != MEM_COMMIT) {
        return std::unexpected(WriteFailure{"memory-write target is not committed", false});
    }

    const auto region_begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (address < region_begin || bytes.size() > region.RegionSize - (address - region_begin)) {
        return std::unexpected(WriteFailure{"memory write crosses a protection region", false});
    }

    const auto writable = is_executable(region.Protect) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD original_protection{};
    auto* destination = reinterpret_cast<void*>(address);
    if (!VirtualProtect(destination, bytes.size(), writable, &original_protection)) {
        return std::unexpected(WriteFailure{windows_error("VirtualProtect(write)"), false});
    }

    std::memcpy(destination, bytes.data(), bytes.size());
    bool failed = false;
    std::string failure;
    if (!FlushInstructionCache(GetCurrentProcess(), destination, bytes.size())) {
        failed = true;
        failure = windows_error("FlushInstructionCache");
    }

    DWORD ignored{};
    if (!VirtualProtect(destination, bytes.size(), original_protection, &ignored)) {
        if (failed) {
            failure += "; ";
        }
        failure += windows_error("VirtualProtect(restore)");
        failed = true;
    }

    if (std::memcmp(destination, bytes.data(), bytes.size()) != 0) {
        if (failed) {
            failure += "; ";
        }
        failure += "memory verification failed";
        failed = true;
    }

    if (failed) {
        return std::unexpected(WriteFailure{std::move(failure), true});
    }
    return {};
}

DataAllocation::DataAllocation(DataAllocation&& other) noexcept {
    *this = std::move(other);
}

DataAllocation& DataAllocation::operator=(DataAllocation&& other) noexcept {
    if (this != &other) {
        reset();
        allocation_ = std::exchange(other.allocation_, nullptr);
        address_ = std::exchange(other.address_, 0);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

DataAllocation::~DataAllocation() {
    reset();
}

std::expected<DataAllocation, std::string> DataAllocation::create(std::size_t size, std::size_t alignment,
                                                                  const ImageContext& image,
                                                                  const AllocationProximity* proximity) {
    if (size == 0 || alignment == 0 || !std::has_single_bit(alignment) ||
        size > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::unexpected("invalid data-allocation size or alignment");
    }

    const auto allocation_size = size + alignment - 1;
    void* allocation{};
    if (proximity == nullptr) {
        allocation = VirtualAlloc(nullptr, allocation_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    } else {
        if (!image.contains_rva(proximity->anchor_rva, 1) ||
            image.base > std::numeric_limits<std::uintptr_t>::max() - proximity->anchor_rva) {
            return std::unexpected("data-allocation near RVA is outside the target image");
        }
        allocation =
            allocate_near(image.base + proximity->anchor_rva, proximity->maximum_distance, allocation_size, alignment);
    }

    if (allocation == nullptr) {
        return std::unexpected(windows_error("VirtualAlloc"));
    }

    DataAllocation result;
    result.allocation_ = allocation;
    result.address_ = align_up(reinterpret_cast<std::uintptr_t>(allocation), alignment);
    result.size_ = size;
    return result;
}

void DataAllocation::reset() noexcept {
    if (allocation_ != nullptr) {
        VirtualFree(allocation_, 0, MEM_RELEASE);
    }
    allocation_ = nullptr;
    address_ = 0;
    size_ = 0;
}

} // namespace fusioncutter::patching_detail
