#pragma once

#include "schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <intrin.h>
#include <span>
#include <type_traits>

namespace fusioncutter::patches::network_diagnostics::trace {

inline constexpr std::size_t kPayloadCapacity = 96;

// Carries one bounded semantic observation from a game callback to the ETL writer.
struct Record {
    std::uint64_t timestamp;
    std::uint64_t sequence;
    std::uint32_t thread_id;
    std::uint32_t context;
    RecordKind kind;
    std::uint16_t flags;
    std::uint16_t processor;
    Carrier carrier;
    std::uint8_t payload_size;
    std::array<std::byte, kPayloadCapacity> payload;
};

struct Stamp {
    std::uint64_t timestamp;
    std::uint16_t processor;
};

[[nodiscard]] inline Stamp read_stamp() noexcept {
    _mm_lfence();
    unsigned int processor{};
    const auto value = __rdtscp(&processor);
    _mm_lfence();
    return {value, static_cast<std::uint16_t>(processor)};
}

static_assert(std::is_trivially_copyable_v<Record>);
static_assert(sizeof(Record) == 128);

template <typename Payload>
    requires(std::is_trivially_copyable_v<Payload> && sizeof(Payload) <= kPayloadCapacity)
[[nodiscard]] std::span<const std::byte> payload_bytes(const Payload& payload) noexcept {
    return std::as_bytes(std::span{&payload, std::size_t{1}});
}

} // namespace fusioncutter::patches::network_diagnostics::trace
