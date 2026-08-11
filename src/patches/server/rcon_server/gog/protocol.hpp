#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace fusioncutter::patches::rcon_server::gog::protocol {

enum class FrameState {
    Incomplete,
    Complete,
    Invalid,
};

struct RequestFrame {
    FrameState state;
    std::size_t consumed{};
    std::string_view command;
};

// Decodes one command from the client's receive buffer.
[[nodiscard]] RequestFrame decode_request(std::span<const char> input) noexcept;

// Encodes a command result or server notification for RCON clients.
[[nodiscard]] std::vector<char> encode_message(std::string_view message);

} // namespace fusioncutter::patches::rcon_server::gog::protocol
