#pragma once

#include <fc/native/values.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>

namespace fc {

// Copy a native field at a signed byte offset; T must be trivially copyable and support T{}.
// The caller supplies readable storage for sizeof(T) bytes.
template <class T> T read_field(const void *object, std::ptrdiff_t offset = 0) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    std::memcpy(&value, static_cast<const std::byte *>(object) + offset, sizeof(T));
    return value;
}

template <class T> T read_field(std::uintptr_t address, std::ptrdiff_t offset = 0) noexcept {
    return read_field<T>(reinterpret_cast<const void *>(address), offset);
}

// Copy a value into a native field at a signed byte offset; the caller supplies writable storage for sizeof(T) bytes.
template <class T> void write_field(void *object, std::ptrdiff_t offset, const T &value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(static_cast<std::byte *>(object) + offset, &value, sizeof(T));
}

template <class T> void write_field(std::uintptr_t address, std::ptrdiff_t offset, const T &value) noexcept {
    write_field(reinterpret_cast<void *>(address), offset, value);
}

// Derive one zero-based array element; clear the parent evidence and mark bounds or RVA overflow as invalid.
template <class T, std::size_t Count> DataLocation<T> element(const DataLocation<T, Count> &array, std::size_t index) {
    DataLocation<T> result{array.rva, array.name, {}, array.invalid || index >= Count || index > UINT32_MAX / sizeof(T)};
    if (!result.invalid) {
        const auto shift = index * sizeof(T);
        result.invalid = shift > UINT32_MAX - array.rva;
        if (!result.invalid) {
            result.rva += static_cast<std::uint32_t>(shift);
        }
    }
    return result;
}

// Shift a typed location by signed bytes, clearing evidence and retaining any invalid derivation.
template <class T, std::size_t Count> DataLocation<T, Count> offset(const DataLocation<T, Count> &location, std::int64_t bytes) {
    auto result = location;
    result.evidence = {};
    if (bytes < 0) {
        // Form the magnitude without negating INT64_MIN, then check subtraction against the current RVA.
        const auto magnitude = static_cast<std::uint64_t>(-(bytes + 1)) + 1;
        result.invalid |= magnitude > result.rva;
        if (!result.invalid) {
            result.rva -= static_cast<std::uint32_t>(magnitude);
        }
    } else {
        result.invalid |= static_cast<std::uint64_t>(bytes) > UINT32_MAX - result.rva;
        if (!result.invalid) {
            result.rva += static_cast<std::uint32_t>(bytes);
        }
    }
    return result;
}

// Derive a function-pointer slot at a zero-based index; the result needs its own evidence.
template <class Function> DataLocation<Function> vtable_slot(const VtableLocation &table, std::size_t index) {
    DataLocation<Function> result{table.rva, table.name, {}, table.invalid || index > UINT32_MAX / sizeof(Function)};
    if (!result.invalid) {
        const auto shift = index * sizeof(Function);
        result.invalid = shift > UINT32_MAX - result.rva;
        if (!result.invalid) {
            result.rva += static_cast<std::uint32_t>(shift);
        }
    }
    return result;
}

// Construct a fixed byte array from literal byte values for native evidence or replacement data.
template <std::uint8_t... Bytes> constexpr auto byte_array() {
    return std::array<std::byte, sizeof...(Bytes)>{std::byte{Bytes}...};
}

// Copy a trivially copyable value's byte representation at a compile-time byte offset inside the array.
template <std::size_t Offset, class T, class Byte, std::size_t N> void write_value(std::array<Byte, N> &bytes, T value) {
    static_assert(std::is_trivially_copyable_v<T> && Offset <= N && sizeof(T) <= N - Offset && sizeof(Byte) == 1);
    std::memcpy(bytes.data() + Offset, &value, sizeof(T));
}

// Return the byte displacement target minus base as signed T, or nullopt if it cannot fit.
// For a relative instruction or field, pass its end address as base.
template <class T> std::optional<T> relative_displacement(std::uintptr_t base, std::uintptr_t target) {
    static_assert(std::is_signed_v<T>);
    // Unsigned subtraction preserves the magnitude even when the distance exceeds the signed address range.
    const auto magnitude = target >= base ? target - base : base - target;
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<T>::max()) + (target < base);
    if (magnitude > limit) {
        return std::nullopt;
    }
    // The minimum negative value has no positive counterpart representable in T.
    if (target < base && magnitude == limit) {
        return std::numeric_limits<T>::min();
    }
    return target >= base ? static_cast<T>(magnitude) : static_cast<T>(-static_cast<std::int64_t>(magnitude));
}

} // namespace fc
