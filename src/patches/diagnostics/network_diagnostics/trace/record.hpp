#pragma once

#include "schema.hpp"

#include <cstddef>
#include <cstdint>
#include <intrin.h>
#include <span>
#include <type_traits>

namespace fusioncutter::patches::network_diagnostics::trace {

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

template <typename Payload>
    requires(std::is_trivially_copyable_v<Payload> && sizeof(Payload) <= diagnostics::kMaximumEtlPayloadSize)
[[nodiscard]] std::span<const std::byte> payload_bytes(const Payload& payload) noexcept {
    return std::as_bytes(std::span{&payload, std::size_t{1}});
}

} // namespace fusioncutter::patches::network_diagnostics::trace
