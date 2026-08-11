#include "protocol.hpp"

#include <cstdint>
#include <cstring>

namespace fusioncutter::patches::rcon_server::gog::protocol {
namespace {

constexpr std::string_view kProtocolLimitError = "RCON response exceeds protocol limit";

} // namespace

RequestFrame decode_request(std::span<const char> input) noexcept {
    if (input.size() < 2) {
        return {FrameState::Incomplete};
    }

    const auto rows = static_cast<std::uint8_t>(input[0]);
    const auto size = static_cast<std::uint8_t>(input[1]);
    if (rows != 1 || size == 0) {
        return {FrameState::Invalid};
    }

    const auto frame_size = static_cast<std::size_t>(size) + 2;
    if (input.size() < frame_size) {
        return {FrameState::Incomplete};
    }

    const auto* command = input.data() + 2;
    if (command[size - 1] != '\0' || std::memchr(command, '\0', size - 1) != nullptr) {
        return {FrameState::Invalid};
    }
    return {FrameState::Complete, frame_size, {command, static_cast<std::size_t>(size - 1)}};
}

std::vector<char> encode_message(std::string_view message) {
    std::vector<std::string_view> rows;
    for (std::size_t offset = 0; offset < message.size();) {
        const auto separator = message.find('\n', offset);
        auto row =
            message.substr(offset, separator == std::string_view::npos ? message.size() - offset : separator - offset);
        if (!row.empty() && row.back() == '\r') {
            row.remove_suffix(1);
        }
        if (row.size() >= 0xFF || rows.size() == 0xFF) {
            return encode_message(kProtocolLimitError);
        }
        rows.push_back(row);
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1;
    }

    std::vector<char> output;
    output.reserve(message.size() + rows.size() * 2 + 1);
    output.push_back(static_cast<char>(rows.size()));
    for (const auto row : rows) {
        output.push_back(static_cast<char>(row.size() + 1));
        output.insert(output.end(), row.begin(), row.end());
        output.push_back('\0');
    }
    return output;
}

} // namespace fusioncutter::patches::rcon_server::gog::protocol
