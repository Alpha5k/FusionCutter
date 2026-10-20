#include "messages.hpp"

#include <charconv>
#include <system_error>

namespace fc::core_plugin::colored_chats {

std::optional<std::uint32_t> parse_color(std::string_view text) noexcept {
    if (text.size() != 6) {
        return std::nullopt;
    }

    std::uint32_t rgb{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rgb, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return 0xff00'0000 | rgb;
}

const wchar_t *one_line(const wchar_t *text, std::array<wchar_t, kChatCapacity> &buffer) noexcept {
    if (!text) {
        return nullptr;
    }

    for (std::size_t index = 0; index < buffer.size() - 1; ++index) {
        const auto character = text[index];
        if (character == L'\0') {
            buffer[index] = L'\0';
            return buffer.data();
        }
        buffer[index] = character == L'\n' ? L' ' : character;
    }
    buffer.back() = L'\0';
    return buffer.data();
}

std::uint32_t message_color(const Colors &colors, std::uint32_t sender, std::uint32_t channel, bool buddy) noexcept {
    return sender == kAdminSender ? colors.admin : buddy ? colors.buddy : channel == 1 ? colors.team : colors.chat;
}

} // namespace fc::core_plugin::colored_chats
