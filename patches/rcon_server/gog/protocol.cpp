#include "protocol.hpp"

#include <cstdint>
#include <cstring>

namespace fc::core_plugin::rcon_server::gog::protocol {
namespace {

constexpr std::string_view kProtocolLimitError = "RCON response exceeds protocol limit";

} // namespace

RequestFrame decode_request(std::span<const char> input) noexcept {
    if (input.size() < 2) {
        return {FrameState::Incomplete};
    }

    // Requests contain one row; its one-byte length includes the final null terminator.
    const auto rows = static_cast<std::uint8_t>(input[0]);
    const auto size = static_cast<std::uint8_t>(input[1]);
    if (rows != 1 || size == 0) {
        return {FrameState::Invalid};
    }

    const auto frame_size = static_cast<std::size_t>(size) + 2;
    if (input.size() < frame_size) {
        return {FrameState::Incomplete};
    }

    // Require the declared terminator at the end so embedded nulls cannot shorten the interpreted command.
    const auto *command = input.data() + 2;
    if (command[size - 1] != '\0' || std::memchr(command, '\0', size - 1) != nullptr) {
        return {FrameState::Invalid};
    }
    return {FrameState::Complete, frame_size, {command, static_cast<std::size_t>(size - 1)}};
}

std::vector<char> encode_message(std::string_view message) {
    // Split on LF and remove one trailing CR from each row; a final LF does not add an empty row.
    std::vector<char> output{'\0'};
    std::size_t row_count{};
    for (std::size_t offset = 0; offset < message.size();) {
        const auto separator = message.find('\n', offset);
        auto row = message.substr(offset, separator == std::string_view::npos ? message.size() - offset : separator - offset);
        if (!row.empty() && row.back() == '\r') {
            row.remove_suffix(1);
        }
        // One-byte lengths include the terminator, leaving at most 254 content bytes and 255 rows.
        if (row.size() >= 0xff || row_count == 0xff) {
            return encode_message(kProtocolLimitError);
        }
        output.push_back(static_cast<char>(row.size() + 1));
        output.insert(output.end(), row.begin(), row.end());
        output.push_back('\0');
        ++row_count;
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1;
    }

    // Fill the count after emitting all rows; empty output retains its zero count.
    output[0] = static_cast<char>(row_count);
    return output;
}

} // namespace fc::core_plugin::rcon_server::gog::protocol
