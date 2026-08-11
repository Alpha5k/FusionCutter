#pragma once

#include "protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fusioncutter::patches::direct_transport {

using SessionKey = std::array<std::uint8_t, 16>;

// Supplies random bytes without coupling security logic to one system provider.
struct RandomSource {
    void* context{};
    bool (*fill)(void* context, std::uint8_t* output, std::size_t bytes) noexcept {};
};

// Supplies secure random bytes and collision-free nonzero connection IDs.
[[nodiscard]] RandomSource system_random_source() noexcept;
[[nodiscard]] bool fill_random(const RandomSource& source, std::span<std::uint8_t> output) noexcept;
[[nodiscard]] bool generate_connection_id(const RandomSource& source, std::span<const std::uint32_t> live_ids,
                                          std::uint32_t& output) noexcept;

// Computes the SipHash-2-4 primitive used by Direct Transport authentication.
[[nodiscard]] std::uint64_t sip_hash24(std::span<const std::uint8_t, 16> key,
                                       std::span<const std::uint8_t> message) noexcept;
// Computes and verifies the direction-bound authentication tag for one direct datagram.
[[nodiscard]] std::uint64_t compute_authentication_tag(Direction direction, std::span<const std::uint8_t, 16> key,
                                                       std::span<const std::uint8_t> serialized_header,
                                                       std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] bool verify_authentication_tag(Direction direction, std::span<const std::uint8_t, 16> key,
                                             std::span<const std::uint8_t> serialized_header,
                                             std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] bool constant_time_equal(std::span<const std::uint8_t> left,
                                       std::span<const std::uint8_t> right) noexcept;

enum class ReplayResult : std::uint8_t {
    AcceptedFirst,
    AcceptedNew,
    AcceptedReordered,
    Duplicate,
    Stale,
    InvalidJump,
};

// Admits bounded packet reordering while rejecting duplicate, stale, and implausibly distant sequences.
class ReplayWindow {
  public:
    static constexpr std::uint32_t kWidth = 256;

    // Classifies and records one authenticated datagram sequence.
    [[nodiscard]] ReplayResult admit(std::uint32_t sequence) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint32_t frontier() const noexcept;

  private:
    void advance(std::uint32_t distance) noexcept;

    std::array<std::uint64_t, 4> seen_{};
    std::uint32_t frontier_{};
    bool initialized_{};
};

} // namespace fusioncutter::patches::direct_transport
