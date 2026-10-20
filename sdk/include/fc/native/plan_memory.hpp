#pragma once

#include <fc/native/plan.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace fc {

template <class T> inline Result<T> Plan::read(Location location, Evidence evidence) const {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    detail::NativeError error;
    // Snapshot reads copy bytes now and retain alignment/evidence claims for validation before installation.
    const FC_ReadClaim claim{location.view(), evidence.view(), sizeof(T), alignof(T), 0, FC_READ_SNAPSHOT, 0, nullptr, {}};
    if (!plan_ || !plan_->read || !plan_->read(plan_->context, &claim, &value, error.sink())) {
        return std::unexpected(error.failure());
    }
    return value;
}

template <class T> inline Result<T> Plan::read(std::uint32_t rva, Evidence evidence) const {
    return read<T>(at(rva), std::move(evidence));
}

template <class T, std::size_t Count> inline auto Plan::read(const DataLocation<T, Count> &site) const {
    using Value = std::conditional_t<Count == 1, T, std::array<T, Count>>;
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    return read<Value>(std::move(location), site.evidence);
}

template <class T> inline void Plan::write(Location location, const T &value, Evidence evidence) const {
    static_assert(std::is_trivially_copyable_v<T>);
    // Submission copies the object's complete byte representation before this call's input expires.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_WRITE;
    operation.value.write = {
        location.view(), evidence.view(), {reinterpret_cast<const std::uint8_t *>(&value), sizeof(T)}, alignof(T), 0, 0, 0};
    submit(operation);
}

template <class T> inline void Plan::write(std::uint32_t rva, const T &value, Evidence evidence) const {
    write(at(rva), value, std::move(evidence));
}

template <class T, std::size_t Count>
inline void Plan::write(const DataLocation<T, Count> &site, const std::conditional_t<Count == 1, T, std::array<T, Count>> &value) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    write(std::move(location), value, site.evidence);
}

inline void Plan::write(Location location, Bytes bytes, Evidence evidence) const {
    FC_Operation operation{};
    operation.kind = FC_OPERATION_WRITE;
    operation.value.write = {
        location.view(), evidence.view(), {reinterpret_cast<const std::uint8_t *>(bytes.data()), detail::count(bytes.size())}, 1, 0, 0, 0};
    submit(operation);
}

template <class T> inline Data<T> Plan::allocate(std::size_t count, Placement options) const {
    return allocate_bytes<T>(count, {}, options);
}

template <class T> inline Data<T> Plan::allocate(std::span<const T> values, Placement options) const {
    return allocate_bytes<T>(values.size(), std::as_bytes(values), options);
}

template <class T, std::size_t N> inline Data<T> Plan::allocate(const std::array<T, N> &values, Placement options) const {
    return allocate<T>(std::span<const T>{values}, options);
}

template <class T, std::size_t N> inline Data<T> Plan::allocate(const T (&values)[N], Placement options) const {
    return allocate<T>(std::span<const T>{values}, options);
}

template <class T> inline Memory<T> Plan::access(Location location, std::size_t count, Evidence evidence) const {
    FC_Operation operation{};
    operation.kind = FC_OPERATION_READ;
    // Check multiplication before converting an element count into the runtime's byte range.
    if (count > UINT64_MAX / sizeof(T)) {
        fail("Memory access byte size exceeds UINT64_MAX");
        return {};
    }
    // Const element types reserve only read access; writable handles reserve read/write permission.
    operation.value.read = {
        location.view(), evidence.view(), count * sizeof(T), alignof(T), !std::is_const_v<T>, FC_READ_MEMORY, 0, nullptr, {}};
    return {submit(operation), api(), count};
}

template <class T> inline Memory<T> Plan::access(const Data<std::remove_const_t<T>> &data) const {
    // Link continuing access to the allocation identity; its address is assigned later during preparation.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_READ;
    operation.value.read.size = data.size() * sizeof(T);
    operation.value.read.alignment = alignof(T);
    operation.value.read.writable = !std::is_const_v<T>;
    operation.value.read.mode = FC_READ_MEMORY;
    operation.value.read.allocation = data.resource();
    return {submit(operation), api(), data.size()};
}

template <class T, std::size_t Count> inline Memory<T> Plan::access(const DataLocation<T, Count> &site) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    return access<T>(std::move(location), Count, site.evidence);
}

template <class T> inline Memory<T> Plan::access(std::uint32_t rva, std::size_t count, Evidence evidence) const {
    return access<T>(at(rva), count, std::move(evidence));
}

template <class T> inline Result<std::vector<T>> Plan::read_array(Location location, std::size_t count, Evidence evidence) const {
    static_assert(std::is_trivially_copyable_v<T>);
    // Snapshot arrays must be nonempty and fit the runtime's 32-bit byte extent.
    if (!count || count > UINT32_MAX / sizeof(T)) {
        fail("Snapshot array is empty or its byte size exceeds UINT32_MAX");
        return std::unexpected(Error{"Snapshot array is empty or its byte size exceeds UINT32_MAX"});
    }

    std::vector<T> values(count);
    detail::NativeError error;
    const FC_ReadClaim claim{location.view(), evidence.view(), count * sizeof(T), alignof(T), 0, FC_READ_SNAPSHOT, 0, nullptr, {}};
    if (!plan_ || !plan_->read || !plan_->read(plan_->context, &claim, values.data(), error.sink())) {
        return std::unexpected(error.failure());
    }
    return values;
}

template <class T> inline Result<std::vector<T>> Plan::read_array(std::uint32_t rva, std::size_t count, Evidence evidence) const {
    return read_array<T>(at(rva), count, std::move(evidence));
}

inline Result<std::vector<std::byte>> Plan::read_bytes(Location location, std::size_t count, Evidence evidence) const {
    return read_array<std::byte>(std::move(location), count, std::move(evidence));
}

inline void Plan::check(Location location, std::size_t count, Evidence evidence) const {
    static_cast<void>(read_bytes(std::move(location), count, std::move(evidence)));
}

inline void Plan::check(std::uint32_t rva, std::size_t count, Evidence evidence) const {
    check(at(rva), count, std::move(evidence));
}

inline void Plan::check(const CodeLocation &site, std::size_t count) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    check(std::move(location), count, site.evidence);
}

template <class T, std::size_t Extent> inline void Plan::write(Location location, std::span<T, Extent> values, Evidence evidence) const {
    static_assert(std::is_trivially_copyable_v<T>);
    FC_Operation operation{};
    operation.kind = FC_OPERATION_WRITE;
    operation.value.write = {location.view(),
                             evidence.view(),
                             {reinterpret_cast<const std::uint8_t *>(values.data()), detail::count(values.size_bytes())},
                             alignof(T),
                             0,
                             0,
                             0};
    submit(operation);
}

inline void Plan::write(const CodeLocation &site, Bytes values) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;

    FC_Operation operation{};
    operation.kind = FC_OPERATION_WRITE;
    // The instruction flag requires the replaced range to end at an original instruction boundary.
    operation.value.write = {location.view(),
                             site.evidence.view(),
                             {reinterpret_cast<const std::uint8_t *>(values.data()), detail::count(values.size())},
                             1,
                             1,
                             0,
                             0};
    submit(operation);
}

inline Result<std::vector<std::byte>> Plan::read_bytes(std::uint32_t rva, std::size_t count, Evidence evidence) const {
    return read_bytes(at(rva), count, std::move(evidence));
}

template <class T>
inline Data<T> Plan::allocate_bytes(std::size_t count, std::span<const std::byte> bytes, const Placement &options) const {
    static_assert(std::is_trivially_copyable_v<T>);
    // Preserve element size/count and copy initializer bytes; empty initializers request zero-filled storage.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_ALLOCATE;
    operation.value.allocation = {count,
                                  sizeof(T),
                                  options.alignment ? options.alignment : alignof(T),
                                  detail::text(options.near_image),
                                  {reinterpret_cast<const std::uint8_t *>(bytes.data()), detail::count(bytes.size())}};
    return {submit(operation), api(), count};
}

} // namespace fc
