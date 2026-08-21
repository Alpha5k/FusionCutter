#include "native_address.hpp"

#include <bit>
#include <limits>

namespace fc::planning {

std::optional<std::uintptr_t> decode_rel32_target(std::uintptr_t next, std::int32_t displacement,
                                                  FC_Architecture architecture) noexcept {
    // x86 arithmetic wraps in the process's 32-bit address domain; x64 retains a genuinely signed displacement.
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
    const auto distance = static_cast<std::uintptr_t>(-static_cast<std::int64_t>(displacement));
    if (next < distance) {
        return std::nullopt;
    }
    return next - distance;
}

bool rel32_reachable(std::uintptr_t next, std::uintptr_t target, FC_Architecture architecture) noexcept {
    if (architecture == FC_ARCH_X86) {
        return true;
    }
    if (target >= next) {
        return target - next <= static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max());
    }
    constexpr auto kNegativeCapacity = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
    return next - target <= kNegativeCapacity;
}

std::optional<std::int32_t> encode_rel32(std::uintptr_t next, std::uintptr_t target,
                                         FC_Architecture architecture) noexcept {
    if (!rel32_reachable(next, target, architecture)) {
        return std::nullopt;
    }
    if (architecture == FC_ARCH_X86) {
        const auto bits = static_cast<std::uint32_t>(target) - static_cast<std::uint32_t>(next);
        return std::bit_cast<std::int32_t>(bits);
    }
    if (target >= next) {
        return static_cast<std::int32_t>(target - next);
    }
    return static_cast<std::int32_t>(-static_cast<std::int64_t>(next - target));
}

} // namespace fc::planning
