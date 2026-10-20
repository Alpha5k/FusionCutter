#pragma once

#include <fc/lua.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc::core_plugin::lua_multiplayer::protocol {

// Distinguishes unsupported Lua values from encodings that exceed the payload or a 16-bit field limit.
enum class EncodeError {
    InvalidValue,
    TooLarge
};

// Associates a listener registration ID with its channel name.
struct Listener {
    // Nonzero registration ID identifying the advertised listener.
    std::uint64_t registration;
    // Borrows the channel name while constructing an outgoing announcement.
    std::string_view channel;
};

// Routes an encoded value to an advertised listener.
struct Message {
    // Nonzero registration ID of the destination listener.
    std::uint64_t destination;
    // Borrows encoded value bytes from the received packet.
    fc::Bytes value;
};

// A message packet begins with one kind byte and an eight-byte little-endian registration ID.
inline constexpr std::size_t kMessageHeaderBytes = 9;

// Checks whether the channel is nonempty and valid UTF-8.
bool valid_channel(std::string_view) noexcept;

// Encodes a complete list within max_bytes; the service supplies unique valid channels and nonzero registration IDs.
fc::Result<std::vector<std::byte>> encode_listeners(std::span<const Listener>, std::size_t max_bytes);

// Decodes an owning channel-to-registration map, rejecting malformed fields, duplicates and trailing bytes.
fc::Result<std::map<std::string, std::uint64_t, std::less<>>> decode_listeners(fc::Bytes);

// Prefixes encoded value bytes with a message kind and a known nonzero destination registration ID.
// The complete packet must fit max_bytes; broadcasts can reuse the same value encoding.
fc::Result<std::vector<std::byte>> encode_message(std::uint64_t destination, fc::Bytes value, std::size_t max_bytes);

// Checks the message header and borrows the remaining value bytes; decode_value validates the complete body.
fc::Result<Message> decode_message(fc::Bytes);

// Encodes nil, booleans, finite float32 numbers, byte strings and plain tables with boolean, number or string keys.
// Allows at most 16 table levels; max_bytes excludes kMessageHeaderBytes. A pending frame error prevents success.
std::expected<std::vector<std::byte>, EncodeError> encode_value(fc::lua::Value, std::size_t max_bytes);

// Decodes one complete value into this frame; pending frame errors, duplicates, unsupported numbers and malformed bytes fail.
fc::Result<fc::lua::Value> decode_value(fc::lua::Frame &, fc::Bytes);

} // namespace fc::core_plugin::lua_multiplayer::protocol
