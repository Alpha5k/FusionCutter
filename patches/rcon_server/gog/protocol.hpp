#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace fc::core_plugin::rcon_server::gog::protocol {

enum class FrameState {
    Incomplete,
    Complete,
    Invalid,
};

// Reports one request at the front of the supplied input.
struct RequestFrame {
    FrameState state;
    // Total frame bytes, including count, length and terminator, when state is Complete.
    std::size_t consumed{};
    // Borrows the input buffer and excludes the terminating null.
    std::string_view command;
};

// Decodes the first legacy RCON request, distinguishing a partial frame from malformed input.
// Keep the input storage unchanged while using the returned command view.
RequestFrame decode_request(std::span<const char> input) noexcept;

// Encodes newline-separated output as legacy rows, limited to 254 content bytes per row and 255 rows per packet.
// Replaces oversized output with a protocol error message.
std::vector<char> encode_message(std::string_view message);

} // namespace fc::core_plugin::rcon_server::gog::protocol
