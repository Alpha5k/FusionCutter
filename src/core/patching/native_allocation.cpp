#include "native_allocation.hpp"

#include <Windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace fc::patching {
namespace {

// Allocation arithmetic is overflow-aware because addresses and sizes may originate in validated plugin plans.
[[nodiscard]] std::string windows_error(std::string_view operation) {
    return std::string{operation} + " failed with Windows error " + std::to_string(GetLastError());
}

[[nodiscard]] std::uintptr_t align_down(std::uintptr_t value, std::size_t alignment) noexcept {
    return value - value % alignment;
}

[[nodiscard]] std::optional<std::uintptr_t> align_up(std::uintptr_t value, std::size_t alignment) noexcept {
    const auto remainder = value % alignment;
    if (remainder == 0) {
        return value;
    }
    const auto adjustment = alignment - remainder;
    return value <= std::numeric_limits<std::uintptr_t>::max() - adjustment
               ? std::optional<std::uintptr_t>{value + adjustment}
               : std::nullopt;
}

[[nodiscard]] bool within_distance(std::uintptr_t left, std::uintptr_t right, std::size_t maximum) noexcept {
    return left >= right ? left - right <= maximum : right - left <= maximum;
}

// Accepts a Windows reservation only when its aligned usable address satisfies the caller's reach constraint.
[[nodiscard]] void* try_allocate_at(std::uintptr_t candidate, std::size_t reservation_size, std::size_t alignment,
                                    NearConstraint constraint) noexcept {
    auto* reservation =
        VirtualAlloc(reinterpret_cast<void*>(candidate), reservation_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (reservation == nullptr) {
        return nullptr;
    }
    const auto aligned = align_up(reinterpret_cast<std::uintptr_t>(reservation), alignment);
    if (aligned && within_distance(*aligned, constraint.reference, constraint.maximum_distance)) {
        return reservation;
    }
    VirtualFree(reservation, 0, MEM_RELEASE);
    return nullptr;
}

// Searches symmetrically by allocation granularity so ASLR gaps near the encoding site are considered first.
[[nodiscard]] void* allocate_near(std::size_t reservation_size, std::size_t alignment,
                                  NearConstraint constraint) noexcept {
    // Clamp the reachable interval to Windows' user address space before deriving granularity-aligned probes.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::size_t>(info.dwAllocationGranularity);
    const auto minimum = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    constraint.reference = std::clamp(constraint.reference, minimum, maximum);
    const auto first = align_down(constraint.reference, granularity);
    const auto lower = constraint.reference > constraint.maximum_distance
                           ? std::max(minimum, constraint.reference - constraint.maximum_distance)
                           : minimum;
    const auto upper = maximum - constraint.reference > constraint.maximum_distance
                           ? constraint.reference + constraint.maximum_distance
                           : maximum;

    // Alternate below and above the reference so the first suitable ASLR gap is also the nearest suitable gap.
    for (std::size_t distance = 0;;) {
        const bool below = first >= lower && distance <= first - lower;
        const bool above = first <= upper && distance <= upper - first;
        if (below) {
            if (auto* result = try_allocate_at(first - distance, reservation_size, alignment, constraint)) {
                return result;
            }
        }
        if (distance != 0 && above) {
            if (auto* result = try_allocate_at(first + distance, reservation_size, alignment, constraint)) {
                return result;
            }
        }
        if ((!below && !above) || distance > std::numeric_limits<std::size_t>::max() - granularity) {
            return nullptr;
        }
        distance += granularity;
    }
}

} // namespace

NativeAllocation::NativeAllocation(NativeAllocation&& other) noexcept
    : reservation_(std::exchange(other.reservation_, nullptr)),
      reservation_size_(std::exchange(other.reservation_size_, 0)), address_(std::exchange(other.address_, 0)),
      size_(std::exchange(other.size_, 0)) {}

NativeAllocation& NativeAllocation::operator=(NativeAllocation&& other) noexcept {
    if (this != &other) {
        reset();
        reservation_ = std::exchange(other.reservation_, nullptr);
        reservation_size_ = std::exchange(other.reservation_size_, 0);
        address_ = std::exchange(other.address_, 0);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

NativeAllocation::~NativeAllocation() {
    reset();
}

std::expected<NativeAllocation, std::string> NativeAllocation::create(std::size_t byte_size, std::size_t alignment,
                                                                      std::optional<NearConstraint> constraint) {
    // Reserve alignment slack because VirtualAlloc guarantees page alignment, not arbitrary caller alignment.
    if (byte_size == 0 || alignment == 0 || !std::has_single_bit(alignment) ||
        byte_size > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::unexpected("Native allocation has an invalid size or alignment");
    }
    const auto reservation_size = byte_size + alignment - 1;
    void* reservation = constraint ? allocate_near(reservation_size, alignment, *constraint)
                                   : VirtualAlloc(nullptr, reservation_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (reservation == nullptr) {
        return std::unexpected(windows_error(constraint ? "VirtualAlloc near native target" : "VirtualAlloc"));
    }
    // Keep the original reservation for VirtualFree while exposing only the aligned subrange to patch code.
    const auto aligned = align_up(reinterpret_cast<std::uintptr_t>(reservation), alignment);
    if (!aligned) {
        VirtualFree(reservation, 0, MEM_RELEASE);
        return std::unexpected("Native allocation alignment overflows the address domain");
    }
    NativeAllocation result;
    result.reservation_ = reservation;
    result.reservation_size_ = reservation_size;
    result.address_ = *aligned;
    result.size_ = byte_size;
    return result;
}

std::expected<void, std::string> NativeAllocation::initialize(std::span<const std::byte> bytes) {
    if (reservation_ == nullptr || bytes.size() > size_) {
        return std::unexpected("Native allocation initialization exceeds its reserved storage");
    }
    if (!bytes.empty()) {
        std::memcpy(reinterpret_cast<void*>(address_), bytes.data(), bytes.size());
    }
    return {};
}

std::expected<void, std::string> NativeAllocation::make_executable() {
    if (reservation_ == nullptr) {
        return std::unexpected("Native allocation is empty");
    }
    DWORD previous{};
    if (!VirtualProtect(reservation_, reservation_size_, PAGE_EXECUTE_READ, &previous)) {
        return std::unexpected(windows_error("VirtualProtect executable allocation"));
    }
    // Flush only the initialized logical range; alignment slack is never an executable code target.
    if (!FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(address_), size_)) {
        return std::unexpected(windows_error("FlushInstructionCache executable allocation"));
    }
    return {};
}

std::uintptr_t NativeAllocation::address() const noexcept {
    return address_;
}

std::size_t NativeAllocation::size() const noexcept {
    return size_;
}

void NativeAllocation::reset() noexcept {
    if (reservation_ != nullptr) {
        VirtualFree(reservation_, 0, MEM_RELEASE);
    }
    reservation_ = nullptr;
    reservation_size_ = 0;
    address_ = 0;
    size_ = 0;
}

} // namespace fc::patching
