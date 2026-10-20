#pragma once

#include <fc/abi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fc::patching {

// Bounds the aligned usable allocation address's distance from a reference address.
struct NearConstraint {
    // Absolute process address at the center of the permitted distance range.
    std::uintptr_t reference{};
    // Maximum permitted distance in bytes, in either direction, to the aligned usable address.
    std::size_t maximum_distance{};
};

// Owns a VirtualAlloc reservation and its aligned usable range.
// Callers retain it while native code can reference the storage.
class NativeAllocation final {
  public:
    NativeAllocation() = default;
    NativeAllocation(const NativeAllocation &) = delete;
    NativeAllocation &operator=(const NativeAllocation &) = delete;
    // Transfers the reservation and usable range; other becomes empty.
    NativeAllocation(NativeAllocation &&other) noexcept;
    // Transfers other's storage after releasing this reservation and leaves other empty.
    // Self-assignment preserves the reservation.
    NativeAllocation &operator=(NativeAllocation &&other) noexcept;
    ~NativeAllocation();

    // Allocates nonzero byte_size bytes at a nonzero power-of-two alignment, or returns an error.
    // A near constraint bounds the aligned usable address's distance from reference in bytes.
    static std::expected<NativeAllocation, std::string> create(std::size_t byte_size, std::size_t alignment,
                                                               std::optional<NearConstraint> constraint = std::nullopt);

    // Copies at most size() bytes into writable unpublished storage; remaining bytes retain their current contents.
    [[nodiscard]] std::expected<void, std::string> initialize(std::span<const std::byte> bytes);
    // Changes the complete reservation to execute-read and flushes the instruction cache for the usable range.
    [[nodiscard]] std::expected<void, std::string> make_executable();

    // Returns the aligned usable address and requested byte size; both are zero for an empty allocation.
    std::uintptr_t address() const noexcept;
    std::size_t size() const noexcept;

  private:
    // Releases the original reservation and clears the usable address and sizes.
    void reset() noexcept;

    // VirtualFree requires this original reservation, which may precede the aligned usable address.
    void *reservation_{};
    // Requested usable bytes plus alignment slack, used when changing page protection.
    std::size_t reservation_size_{};
    // Aligned subrange exposed to callers; size_ excludes alignment slack.
    std::uintptr_t address_{};
    std::size_t size_{};
};

// Creates an x64 executable indirect-jump relay within rel32 reach of reference; callers retain it while the jump is used.
std::expected<NativeAllocation, std::string> create_jump_relay(std::uintptr_t target, std::uintptr_t reference);

} // namespace fc::patching
