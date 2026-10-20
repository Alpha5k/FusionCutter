#pragma once

#include <fc/abi.h>

#include <cstddef>
#include <expected>
#include <cstdint>
#include <concepts>
#include <limits>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace fc {

// Owns an operation's error text and an optional Windows error code.
struct Error {
    std::string message;
    std::uint32_t windows_error{};
};
// Returns a value, or the error that prevented the operation from completing.
template <class T = void> using Result = std::expected<T, Error>;

// Read-only view of bytes owned elsewhere; the owner must keep them alive while the view is used.
using Bytes = std::span<const std::byte>;

namespace detail {

// Stable runtime owner/host access may be retained; stage context objects expire after their call.
struct StageAccess {
    void *owner{};
    const FC_Host *host{};
};

// Checks an extensible table before reading a complete field in its published prefix.
template <class T> inline bool table_available(const T *table, std::size_t offset, std::size_t size) noexcept {
    return table && offset <= table->byte_size && size <= table->byte_size - offset;
}

// Throws when a C++ view length cannot fit in the C ABI's 32-bit count.
inline std::uint32_t count(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("View length exceeds the C ABI's 32-bit count limit");
    }
    return static_cast<std::uint32_t>(value);
}

// The caller keeps the source bytes alive while the returned C view is used.
inline FC_Text text(std::string_view value) {
    return {value.data(), count(value.size())};
}

// Lowers a C++ scalar to its C ABI representation; text borrows the supplied value's storage.
template <class T> FC_Value scalar(const T &value) {
    FC_Value result{};
    if constexpr (std::same_as<T, bool>) {
        result.kind = FC_SCALAR_BOOL;
        result.value.boolean = value ? 1u : 0u;
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        result.kind = FC_SCALAR_SIGNED;
        result.value.signed_value = value;
    } else if constexpr (std::is_integral_v<T>) {
        result.kind = FC_SCALAR_UNSIGNED;
        result.value.unsigned_value = value;
    } else if constexpr (std::is_floating_point_v<T>) {
        result.kind = FC_SCALAR_REAL;
        result.value.real = value;
    } else {
        result.kind = FC_SCALAR_TEXT;
        result.value.text = text(value);
    }
    return result;
}

// Converts a validated C view; a zero count permits a null pointer.
inline std::string_view view(FC_Text value) {
    return value.size ? std::string_view{value.data, value.size} : std::string_view{};
}

// Lowercases ASCII letters while preserving all other bytes.
inline char fold_ascii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

// Builds an ASCII lowercase key for case-insensitive lookup containers.
inline std::string folded_id(std::string_view value) {
    std::string result{value};
    for (auto &character : result) {
        character = fold_ascii(character);
    }
    return result;
}

// Compares IDs ASCII case-insensitively while preserving non-ASCII bytes exactly.
inline bool equal_id(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }

    for (std::size_t i = 0; i != a.size(); ++i) {
        if (fold_ascii(a[i]) != fold_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

// Sends error text synchronously; the sink must copy it if it needs to retain it.
inline void report(FC_ErrorSink sink, const Error &error) noexcept {
    if (sink.write) {
        sink.write(sink.context, {error.message.data(), static_cast<std::uint32_t>(error.message.size())}, error.windows_error);
    }
}
} // namespace detail
} // namespace fc
