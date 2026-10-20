#include "native_address.hpp"

#include <bit>
#include <limits>

namespace fc::planning {

std::optional<std::uintptr_t> decode_rel32_target(std::uintptr_t next, std::int32_t displacement, FC_Architecture architecture) noexcept {
    // x86 arithmetic wraps in the 32-bit address space; x64 adds a signed displacement with overflow checks.
    if (architecture == FC_ARCH_X86) {
        const auto wrapped = static_cast<std::uint32_t>(next) + static_cast<std::uint32_t>(displacement);
        return static_cast<std::uintptr_t>(wrapped);
    }

    if (displacement >= 0) {
        const auto distance = static_cast<std::uintptr_t>(displacement);
        if (next > std::numeric_limits<std::uintptr_t>::max() - distance) {
            return std::nullopt;
        }
        return next + distance;
    }

    // Widen before negation so INT32_MIN remains representable as a positive distance.
    const auto distance = static_cast<std::uintptr_t>(-static_cast<std::int64_t>(displacement));
    if (next < distance) {
        return std::nullopt;
    }
    return next - distance;
}

std::optional<std::int32_t> encode_rel32(std::uintptr_t next, std::uintptr_t target, FC_Architecture architecture) noexcept {
    if (architecture == FC_ARCH_X86) {
        // Preserve the wrapping subtraction's bit pattern as a signed displacement.
        const auto bits = static_cast<std::uint32_t>(target) - static_cast<std::uint32_t>(next);
        return std::bit_cast<std::int32_t>(bits);
    }

    // Check each unsigned distance before narrowing; the negative range reaches one byte farther.
    if (target >= next) {
        const auto distance = target - next;
        if (distance > static_cast<std::uintptr_t>(INT32_MAX)) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(distance);
    }
    const auto distance = next - target;
    if (distance > static_cast<std::uint64_t>(INT32_MAX) + 1) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(-static_cast<std::int64_t>(distance));
}

} // namespace fc::planning
