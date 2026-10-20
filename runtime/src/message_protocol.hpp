#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace fc::runtime::message_protocol {

inline constexpr std::size_t kFrameBytes = 2048;

enum class Kind : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    Data = 3,
    Failed = 4,
    Ready = 5
};

// Distinguishes the two ends of one handshake from earlier connections using the same native slot.
struct Tokens {
    std::uint64_t client{}, host{};
};

// Contains the fields decoded from one complete multiplayer frame.
struct ParsedFrame {
    Kind kind{};
    Tokens tokens{};
    // These text and payload views remain valid while the input frame's bytes remain unchanged.
    std::vector<std::string_view> manifest;
    std::string_view feature;
    std::span<const std::byte> payload;
};

// Accepts nonempty, valid UTF-8 IDs.
bool valid_feature_id(std::string_view id) noexcept;

// Parses a complete frame and checks its structure and required nonzero tokens.
// The multiplayer service checks that those tokens belong to the current connection.
std::optional<ParsedFrame> parse(std::span<const std::byte> frame);

// Encodes a strictly sorted manifest for Hello (nonzero client, zero host) or Welcome (two nonzero tokens).
// Returns zero for invalid input or insufficient space.
std::size_t encode_manifest(Kind kind, Tokens tokens, std::span<const std::string_view> manifest, std::span<std::byte> out) noexcept;

// Encodes Ready with two nonzero tokens; returns zero for invalid input or insufficient space.
std::size_t encode_ready(Tokens tokens, std::span<std::byte> out) noexcept;

// Encodes a feature payload with two nonzero tokens; returns zero for invalid input or insufficient space.
std::size_t encode_data(Tokens tokens, std::string_view id, std::span<const std::byte> payload, std::span<std::byte> out) noexcept;

// Encodes Failed with two nonzero tokens; an empty ID fails the connection. Returns zero on encoding failure.
std::size_t encode_failed(Tokens tokens, std::string_view id, std::span<std::byte> out) noexcept;

// Returns the Data payload byte budget for this ID, or zero when the ID cannot fit or is invalid.
std::size_t payload_capacity(std::string_view id) noexcept;

} // namespace fc::runtime::message_protocol
