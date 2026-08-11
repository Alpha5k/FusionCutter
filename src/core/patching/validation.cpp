#include "validation.hpp"

#include "memory.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace fusioncutter::patching_detail {

OutcomeReason operation_failure(std::string message, std::string_view operation) {
    OutcomeReason reason{std::move(message), std::nullopt, std::nullopt};
    if (!operation.empty()) {
        reason.operation = std::string{operation};
    }
    return reason;
}

bool valid_pattern(const OwnedPattern& pattern) noexcept {
    if (pattern.bytes.empty() || (!pattern.mask.empty() && pattern.mask.size() != pattern.bytes.size())) {
        return false;
    }
    return pattern.mask.empty() || std::ranges::any_of(pattern.mask, [](std::byte byte) {
               return byte != std::byte{};
           });
}

bool pattern_matches(std::uintptr_t address, const OwnedPattern& pattern) noexcept {
    if (!readable_memory(address, pattern.bytes.size())) {
        return false;
    }

    const auto* actual = reinterpret_cast<const std::byte*>(address);
    if (pattern.mask.empty()) {
        return std::equal(pattern.bytes.begin(), pattern.bytes.end(), actual);
    }

    for (std::size_t index = 0; index < pattern.bytes.size(); ++index) {
        if ((actual[index] & pattern.mask[index]) != (pattern.bytes[index] & pattern.mask[index])) {
            return false;
        }
    }
    return true;
}

std::expected<std::uintptr_t, std::string> target_address(const ImageContext& image, std::uint32_t rva,
                                                          std::size_t size) {
    if (image.base == 0 || !image.contains_rva(rva, size) ||
        image.base > std::numeric_limits<std::uintptr_t>::max() - rva) {
        return std::unexpected("target RVA is outside the selected image");
    }
    const auto address = image.base + rva;
    if (!readable_memory(address, size)) {
        return std::unexpected("target RVA is not readable committed memory");
    }
    return address;
}

std::string inline_hook_error(const safetyhook::InlineHook::Error& error) {
    switch (error.type) {
    case safetyhook::InlineHook::Error::BAD_ALLOCATION:
        return "SafetyHook could not allocate a trampoline";
    case safetyhook::InlineHook::Error::FAILED_TO_DECODE_INSTRUCTION:
        return "SafetyHook could not decode the target instruction";
    case safetyhook::InlineHook::Error::SHORT_JUMP_IN_TRAMPOLINE:
        return "SafetyHook rejected a short jump in the trampoline";
    case safetyhook::InlineHook::Error::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE:
        return "SafetyHook could not relocate an instruction-pointer-relative instruction";
    case safetyhook::InlineHook::Error::UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE:
        return "SafetyHook cannot relocate an instruction in the hook span";
    case safetyhook::InlineHook::Error::FAILED_TO_UNPROTECT:
        return "SafetyHook could not change target memory protection";
    case safetyhook::InlineHook::Error::NOT_ENOUGH_SPACE:
        return "SafetyHook did not find enough instruction space for the hook";
    default:
        return "SafetyHook reported an unknown inline-hook error";
    }
}

std::string mid_hook_error(const safetyhook::MidHook::Error& error) {
    if (error.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
        return "SafetyHook could not allocate a mid-hook stub";
    }
    if (error.type == safetyhook::MidHook::Error::BAD_INLINE_HOOK) {
        return inline_hook_error(error.inline_hook_error);
    }
    return "SafetyHook reported an unknown mid-hook error";
}

std::vector<std::byte> copy_bytes(std::uintptr_t address, std::size_t size) {
    const auto* first = reinterpret_cast<const std::byte*>(address);
    return {first, first + size};
}

bool equal_memory(std::uintptr_t address, std::span<const std::byte> expected) noexcept {
    return readable_memory(address, expected.size()) &&
           std::memcmp(reinterpret_cast<const void*>(address), expected.data(), expected.size()) == 0;
}

} // namespace fusioncutter::patching_detail
