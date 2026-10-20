#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace fc::reporting {

// Returns the largest prefix within the byte limit ending at a code point boundary when value is valid UTF-8.
inline std::size_t utf8_prefix(std::string_view value, std::size_t limit) noexcept {
    auto result = std::min(limit, value.size());
    // A continuation byte at the cut belongs to the preceding code point; exclude that whole code point.
    while (result != 0 && result < value.size() && (static_cast<unsigned char>(value[result]) & 0xc0U) == 0x80U) {
        --result;
    }

    return result;
}

// Copies a bounded prefix without splitting valid UTF-8, replaces ASCII controls with spaces and returns the byte count.
// No terminator is appended; the returned count delimits the written prefix.
inline std::size_t copy_structured_bytes(std::span<char> destination, std::string_view source) noexcept {
    const auto copied = utf8_prefix(source, destination.size());
    // Normalize ASCII controls without changing the byte lengths of UTF-8 characters.
    for (std::size_t index = 0; index < copied; ++index) {
        const auto byte = static_cast<unsigned char>(source[index]);
        destination[index] = byte <= 0x1fU || byte == 0x7fU ? ' ' : source[index];
    }

    return copied;
}

} // namespace fc::reporting
